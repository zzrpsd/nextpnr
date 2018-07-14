/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2018  Clifford Wolf <clifford@symbioticeda.com>
 *  Copyright (C) 2018  Serge Bazanski  <q3k@symbioticeda.com>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include <cmath>
#include <queue>

#include "log.h"
#include "router1.h"

namespace {

USING_NEXTPNR_NAMESPACE

struct hash_id_wire
{
    std::size_t operator()(const std::pair<IdString, WireId> &arg) const noexcept
    {
        std::size_t seed = std::hash<IdString>()(arg.first);
        seed ^= std::hash<WireId>()(arg.second) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        return seed;
    }
};

struct hash_id_pip
{
    std::size_t operator()(const std::pair<IdString, PipId> &arg) const noexcept
    {
        std::size_t seed = std::hash<IdString>()(arg.first);
        seed ^= std::hash<PipId>()(arg.second) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        return seed;
    }
};

struct QueuedWire
{
    WireId wire;
    PipId pip;

    delay_t delay = 0, togo = 0;
    int randtag = 0;

    struct Greater
    {
        bool operator()(const QueuedWire &lhs, const QueuedWire &rhs) const noexcept
        {
            delay_t l = lhs.delay + lhs.togo, r = rhs.delay + rhs.togo;
            return l == r ? lhs.randtag > rhs.randtag : l > r;
        }
    };
};

struct RipupScoreboard
{
    std::unordered_map<WireId, int> wireScores;
    std::unordered_map<PipId, int> pipScores;
    std::unordered_map<std::pair<IdString, WireId>, int, hash_id_wire> netWireScores;
    std::unordered_map<std::pair<IdString, PipId>, int, hash_id_pip> netPipScores;
};

void ripup_net(MutateContext &proxy, Context *ctx, IdString net_name)
{
    auto net_info = ctx->nets.at(net_name).get();
    std::vector<PipId> pips;
    std::vector<WireId> wires;

    pips.reserve(net_info->wires.size());
    wires.reserve(net_info->wires.size());

    for (auto &it : net_info->wires) {
        if (it.second.pip != PipId())
            pips.push_back(it.second.pip);
        else
            wires.push_back(it.first);
    }

    for (auto pip : pips)
        proxy.unbindPip(pip);

    for (auto wire : wires)
        proxy.unbindWire(wire);

    NPNR_ASSERT(net_info->wires.empty());
}

struct Router
{
    Context *ctx;
    RipupScoreboard scores;
    IdString net_name;

    bool ripup;
    delay_t ripup_penalty;

    std::unordered_set<IdString> rippedNets;
    std::unordered_map<WireId, QueuedWire> visited;
    int visitCnt = 0, revisitCnt = 0, overtimeRevisitCnt = 0;
    bool routedOkay = false;
    delay_t maxDelay = 0.0;
    WireId failedDest;

    void route(MutateContext &proxy, const std::unordered_map<WireId, delay_t> &src_wires, WireId dst_wire)
    {
        std::priority_queue<QueuedWire, std::vector<QueuedWire>, QueuedWire::Greater> queue;

        visited.clear();

        for (auto &it : src_wires) {
            QueuedWire qw;
            qw.wire = it.first;
            qw.pip = PipId();
            qw.delay = it.second;
            qw.togo = ctx->estimateDelay(qw.wire, dst_wire);
            qw.randtag = ctx->rng();

            queue.push(qw);
            visited[qw.wire] = qw;
        }

        int thisVisitCnt = 0;
        int thisVisitCntLimit = 0;

        while (!queue.empty() && (thisVisitCntLimit == 0 || thisVisitCnt < thisVisitCntLimit)) {
            QueuedWire qw = queue.top();
            queue.pop();

            if (thisVisitCntLimit == 0 && visited.count(dst_wire))
                thisVisitCntLimit = (thisVisitCnt * 3) / 2;

            for (auto pip : ctx->getPipsDownhill(qw.wire)) {
                delay_t next_delay = qw.delay + ctx->getPipDelay(pip).avgDelay();
                WireId next_wire = ctx->getPipDstWire(pip);
                bool foundRipupNet = false;
                thisVisitCnt++;

                if (!proxy.checkWireAvail(next_wire)) {
                    if (!ripup)
                        continue;
                    IdString ripupWireNet = proxy.getConflictingWireNet(next_wire);
                    if (ripupWireNet == net_name || ripupWireNet == IdString())
                        continue;

                    auto it1 = scores.wireScores.find(next_wire);
                    if (it1 != scores.wireScores.end())
                        next_delay += (it1->second * ripup_penalty) / 8;

                    auto it2 = scores.netWireScores.find(std::make_pair(ripupWireNet, next_wire));
                    if (it2 != scores.netWireScores.end())
                        next_delay += it2->second * ripup_penalty;

                    foundRipupNet = true;
                }

                if (!proxy.checkPipAvail(pip)) {
                    if (!ripup)
                        continue;
                    IdString ripupPipNet = proxy.getConflictingPipNet(pip);
                    if (ripupPipNet == net_name || ripupPipNet == IdString())
                        continue;

                    auto it1 = scores.pipScores.find(pip);
                    if (it1 != scores.pipScores.end())
                        next_delay += (it1->second * ripup_penalty) / 8;

                    auto it2 = scores.netPipScores.find(std::make_pair(ripupPipNet, pip));
                    if (it2 != scores.netPipScores.end())
                        next_delay += it2->second * ripup_penalty;

                    foundRipupNet = true;
                }

                if (foundRipupNet)
                    next_delay += ripup_penalty;

                NPNR_ASSERT(next_delay >= 0);

                if (visited.count(next_wire)) {
                    if (visited.at(next_wire).delay <= next_delay + ctx->getDelayEpsilon())
                        continue;
#if 0 // FIXME
                    if (ctx->debug)
                        log("Found better route to %s. Old vs new delay "
                            "estimate: %.3f %.3f\n",
                            ctx->getWireName(next_wire).c_str(),
                            ctx->getDelayNS(visited.at(next_wire).delay),
                            ctx->getDelayNS(next_delay));
#endif
                    if (thisVisitCntLimit == 0)
                        revisitCnt++;
                    else
                        overtimeRevisitCnt++;
                }

                QueuedWire next_qw;
                next_qw.wire = next_wire;
                next_qw.pip = pip;
                next_qw.delay = next_delay;
                next_qw.togo = ctx->estimateDelay(next_wire, dst_wire);
                qw.randtag = ctx->rng();

                visited[next_qw.wire] = next_qw;
                queue.push(next_qw);
            }
        }

        visitCnt += thisVisitCnt;
    }

    Router(Context *ctx, RipupScoreboard &scores, WireId src_wire, WireId dst_wire, bool ripup = false,
           delay_t ripup_penalty = 0)
            : ctx(ctx), scores(scores), ripup(ripup), ripup_penalty(ripup_penalty)
    {
        std::unordered_map<WireId, delay_t> src_wires;
        src_wires[src_wire] = 0;
        {
            auto &&proxy = ctx->rwproxy();
            route(proxy, src_wires, dst_wire);
        }
        routedOkay = visited.count(dst_wire);

        if (ctx->debug) {
            log("Route (from destination to source):\n");

            WireId cursor = dst_wire;

            while (1) {
                log("  %8.3f %s\n", ctx->getDelayNS(visited[cursor].delay), ctx->getWireName(cursor).c_str(ctx));

                if (cursor == src_wire)
                    break;

                cursor = ctx->getPipSrcWire(visited[cursor].pip);
            }
        }
    }

    Router(Context *ctx, RipupScoreboard &scores, IdString net_name, bool ripup = false, delay_t ripup_penalty = 0)
            : ctx(ctx), scores(scores), net_name(net_name), ripup(ripup), ripup_penalty(ripup_penalty)
    {
        auto net_info = ctx->nets.at(net_name).get();

        if (ctx->debug)
            log("Routing net %s.\n", net_name.c_str(ctx));

        if (ctx->debug)
            log("  Source: %s.%s.\n", net_info->driver.cell->name.c_str(ctx), net_info->driver.port.c_str(ctx));

        auto src_bel = net_info->driver.cell->bel;

        if (src_bel == BelId())
            log_error("Source cell %s (%s) is not mapped to a bel.\n", net_info->driver.cell->name.c_str(ctx),
                      net_info->driver.cell->type.c_str(ctx));

        if (ctx->debug)
            log("    Source bel: %s\n", ctx->getBelName(src_bel).c_str(ctx));

        IdString driver_port = net_info->driver.port;

        auto driver_port_it = net_info->driver.cell->pins.find(driver_port);
        if (driver_port_it != net_info->driver.cell->pins.end())
            driver_port = driver_port_it->second;

        auto src_wire = ctx->rproxy().getWireBelPin(src_bel, ctx->portPinFromId(driver_port));

        if (src_wire == WireId())
            log_error("No wire found for port %s (pin %s) on source cell %s "
                      "(bel %s).\n",
                      net_info->driver.port.c_str(ctx), driver_port.c_str(ctx), net_info->driver.cell->name.c_str(ctx),
                      ctx->getBelName(src_bel).c_str(ctx));

        if (ctx->debug)
            log("    Source wire: %s\n", ctx->getWireName(src_wire).c_str(ctx));

        std::unordered_map<WireId, delay_t> src_wires;
        src_wires[src_wire] = 0;

        auto &&proxy = ctx->rwproxy();

        ripup_net(proxy, ctx, net_name);
        proxy.bindWire(src_wire, net_name, STRENGTH_WEAK);

        std::vector<PortRef> users_array = net_info->users;
        ctx->shuffle(users_array);

        for (auto &user_it : users_array) {
            if (ctx->debug)
                log("  Route to: %s.%s.\n", user_it.cell->name.c_str(ctx), user_it.port.c_str(ctx));

            auto dst_bel = user_it.cell->bel;

            if (dst_bel == BelId())
                log_error("Destination cell %s (%s) is not mapped to a bel.\n", user_it.cell->name.c_str(ctx),
                          user_it.cell->type.c_str(ctx));

            if (ctx->debug)
                log("    Destination bel: %s\n", ctx->getBelName(dst_bel).c_str(ctx));

            IdString user_port = user_it.port;

            auto user_port_it = user_it.cell->pins.find(user_port);

            if (user_port_it != user_it.cell->pins.end())
                user_port = user_port_it->second;

            auto dst_wire = proxy.getWireBelPin(dst_bel, ctx->portPinFromId(user_port));

            if (dst_wire == WireId())
                log_error("No wire found for port %s (pin %s) on destination "
                          "cell %s (bel %s).\n",
                          user_it.port.c_str(ctx), user_port.c_str(ctx), user_it.cell->name.c_str(ctx),
                          ctx->getBelName(dst_bel).c_str(ctx));

            if (ctx->debug) {
                log("    Destination wire: %s\n", ctx->getWireName(dst_wire).c_str(ctx));
                log("    Path delay estimate: %.2f\n", float(ctx->estimateDelay(src_wire, dst_wire)));
            }

            route(proxy, src_wires, dst_wire);

            if (visited.count(dst_wire) == 0) {
                if (ctx->debug)
                    log("Failed to route %s -> %s.\n", ctx->getWireName(src_wire).c_str(ctx),
                        ctx->getWireName(dst_wire).c_str(ctx));
                else if (ripup)
                    log_info("Failed to route %s -> %s.\n", ctx->getWireName(src_wire).c_str(ctx),
                             ctx->getWireName(dst_wire).c_str(ctx));
                ripup_net(proxy, ctx, net_name);
                failedDest = dst_wire;
                return;
            }

            if (ctx->debug)
                log("    Final path delay: %.3f\n", ctx->getDelayNS(visited[dst_wire].delay));
            maxDelay = fmaxf(maxDelay, visited[dst_wire].delay);

            if (ctx->debug)
                log("    Route (from destination to source):\n");

            WireId cursor = dst_wire;

            while (1) {
                if (ctx->debug)
                    log("    %8.3f %s\n", ctx->getDelayNS(visited[cursor].delay), ctx->getWireName(cursor).c_str(ctx));

                if (src_wires.count(cursor))
                    break;

                IdString conflicting_wire_net = proxy.getConflictingWireNet(cursor);

                if (conflicting_wire_net != IdString()) {
                    NPNR_ASSERT(ripup);
                    NPNR_ASSERT(conflicting_wire_net != net_name);

                    proxy.unbindWire(cursor);
                    if (!proxy.checkWireAvail(cursor))
                        ripup_net(proxy, ctx, conflicting_wire_net);

                    rippedNets.insert(conflicting_wire_net);
                    scores.wireScores[cursor]++;
                    scores.netWireScores[std::make_pair(net_name, cursor)]++;
                    scores.netWireScores[std::make_pair(conflicting_wire_net, cursor)]++;
                }

                PipId pip = visited[cursor].pip;
                IdString conflicting_pip_net = proxy.getConflictingPipNet(pip);

                if (conflicting_pip_net != IdString()) {
                    NPNR_ASSERT(ripup);
                    NPNR_ASSERT(conflicting_pip_net != net_name);

                    proxy.unbindPip(pip);
                    if (!proxy.checkPipAvail(pip))
                        ripup_net(proxy, ctx, conflicting_pip_net);

                    rippedNets.insert(conflicting_pip_net);
                    scores.pipScores[visited[cursor].pip]++;
                    scores.netPipScores[std::make_pair(net_name, visited[cursor].pip)]++;
                    scores.netPipScores[std::make_pair(conflicting_pip_net, visited[cursor].pip)]++;
                }

                proxy.bindPip(visited[cursor].pip, net_name, STRENGTH_WEAK);
                src_wires[cursor] = visited[cursor].delay;
                cursor = ctx->getPipSrcWire(visited[cursor].pip);
            }
        }

        routedOkay = true;
    }
};

} // namespace

NEXTPNR_NAMESPACE_BEGIN

bool router1(Context *ctx)
{
    try {
        int totalVisitCnt = 0, totalRevisitCnt = 0, totalOvertimeRevisitCnt = 0;
        delay_t ripup_penalty = ctx->getRipupDelayPenalty();
        RipupScoreboard scores;

        log_break();
        log_info("Routing..\n");

        std::unordered_set<IdString> netsQueue;

        for (auto &net_it : ctx->nets) {
            auto net_name = net_it.first;
            auto net_info = net_it.second.get();

            if (net_info->driver.cell == nullptr)
                continue;

            if (!net_info->wires.empty())
                continue;

            netsQueue.insert(net_name);
        }

        if (netsQueue.empty()) {
            log_info("found no unrouted nets. no routing necessary.\n");
            return true;
        }

        log_info("found %d unrouted nets. starting routing procedure.\n", int(netsQueue.size()));

        delay_t estimatedTotalDelay = 0.0;
        int estimatedTotalDelayCnt = 0;

        {
            auto &&proxy = ctx->rproxy();
            for (auto net_name : netsQueue) {
                auto net_info = ctx->nets.at(net_name).get();

                auto src_bel = net_info->driver.cell->bel;

                if (src_bel == BelId())
                    continue;

                IdString driver_port = net_info->driver.port;

                auto driver_port_it = net_info->driver.cell->pins.find(driver_port);
                if (driver_port_it != net_info->driver.cell->pins.end())
                    driver_port = driver_port_it->second;

                auto src_wire = proxy.getWireBelPin(src_bel, ctx->portPinFromId(driver_port));

                if (src_wire == WireId())
                    continue;

                for (auto &user_it : net_info->users) {
                    auto dst_bel = user_it.cell->bel;

                    if (dst_bel == BelId())
                        continue;

                    IdString user_port = user_it.port;

                    auto user_port_it = user_it.cell->pins.find(user_port);

                    if (user_port_it != user_it.cell->pins.end())
                        user_port = user_port_it->second;

                    auto dst_wire = proxy.getWireBelPin(dst_bel, ctx->portPinFromId(user_port));

                    if (dst_wire == WireId())
                        continue;

                    estimatedTotalDelay += ctx->estimateDelay(src_wire, dst_wire);
                    estimatedTotalDelayCnt++;
                }
            }
        }

        log_info("estimated total wire delay: %.2f (avg %.2f)\n", float(estimatedTotalDelay),
                 float(estimatedTotalDelay) / estimatedTotalDelayCnt);

        int iterCnt = 0;

        while (!netsQueue.empty()) {
            if (iterCnt == 200) {
                log_warning("giving up after %d iterations.\n", iterCnt);
                log_info("Checksum: 0x%08x\n", ctx->checksum());
#ifndef NDEBUG
                ctx->check();
#endif
                return false;
            }

            iterCnt++;
            if (ctx->verbose)
                log_info("-- %d --\n", iterCnt);

            int visitCnt = 0, revisitCnt = 0, overtimeRevisitCnt = 0, netCnt = 0;

            std::unordered_set<IdString> ripupQueue;

            if (ctx->verbose || iterCnt == 1)
                log_info("routing queue contains %d nets.\n", int(netsQueue.size()));

            bool printNets = ctx->verbose && (netsQueue.size() < 10);

            std::vector<IdString> netsArray(netsQueue.begin(), netsQueue.end());
            ctx->sorted_shuffle(netsArray);
            netsQueue.clear();

            for (auto net_name : netsArray) {
                if (printNets)
                    log_info("  routing net %s. (%d users)\n", net_name.c_str(ctx),
                             int(ctx->nets.at(net_name)->users.size()));

                Router router(ctx, scores, net_name, false);

                netCnt++;
                visitCnt += router.visitCnt;
                revisitCnt += router.revisitCnt;
                overtimeRevisitCnt += router.overtimeRevisitCnt;

                if (!router.routedOkay) {
                    if (printNets)
                        log_info("    failed to route to %s.\n", ctx->getWireName(router.failedDest).c_str(ctx));
                    ripupQueue.insert(net_name);
                }

                if ((ctx->verbose || iterCnt == 1) && !printNets && (netCnt % 100 == 0))
                    log_info("  processed %d nets. (%d routed, %d failed)\n", netCnt, netCnt - int(ripupQueue.size()),
                             int(ripupQueue.size()));
            }

            int normalRouteCnt = netCnt - int(ripupQueue.size());

            if ((ctx->verbose || iterCnt == 1) && (netCnt % 100 != 0))
                log_info("  processed %d nets. (%d routed, %d failed)\n", netCnt, normalRouteCnt,
                         int(ripupQueue.size()));

            if (ctx->verbose)
                log_info("  visited %d PIPs (%.2f%% revisits, %.2f%% overtime "
                         "revisits).\n",
                         visitCnt, (100.0 * revisitCnt) / visitCnt, (100.0 * overtimeRevisitCnt) / visitCnt);

            if (!ripupQueue.empty()) {
                if (ctx->verbose || iterCnt == 1)
                    log_info("failed to route %d nets. re-routing in ripup "
                             "mode.\n",
                             int(ripupQueue.size()));

                printNets = ctx->verbose && (ripupQueue.size() < 10);

                visitCnt = 0;
                revisitCnt = 0;
                overtimeRevisitCnt = 0;
                netCnt = 0;
                int ripCnt = 0;

                std::vector<IdString> ripupArray(ripupQueue.begin(), ripupQueue.end());
                ctx->sorted_shuffle(ripupArray);

                for (auto net_name : ripupArray) {
                    if (printNets)
                        log_info("  routing net %s. (%d users)\n", net_name.c_str(ctx),
                                 int(ctx->nets.at(net_name)->users.size()));

                    Router router(ctx, scores, net_name, true, ripup_penalty);

                    netCnt++;
                    visitCnt += router.visitCnt;
                    revisitCnt += router.revisitCnt;
                    overtimeRevisitCnt += router.overtimeRevisitCnt;

                    if (!router.routedOkay)
                        log_error("Net %s is impossible to route.\n", net_name.c_str(ctx));

                    for (auto it : router.rippedNets)
                        netsQueue.insert(it);

                    if (printNets) {
                        if (router.rippedNets.size() < 10) {
                            log_info("    ripped up %d other nets:\n", int(router.rippedNets.size()));
                            for (auto n : router.rippedNets)
                                log_info("      %s (%d users)\n", n.c_str(ctx), int(ctx->nets.at(n)->users.size()));
                        } else {
                            log_info("    ripped up %d other nets.\n", int(router.rippedNets.size()));
                        }
                    }

                    ripCnt += router.rippedNets.size();

                    if ((ctx->verbose || iterCnt == 1) && !printNets && (netCnt % 100 == 0))
                        log_info("  routed %d nets, ripped %d nets.\n", netCnt, ripCnt);
                }

                if ((ctx->verbose || iterCnt == 1) && (netCnt % 100 != 0))
                    log_info("  routed %d nets, ripped %d nets.\n", netCnt, ripCnt);

                if (ctx->verbose)
                    log_info("  visited %d PIPs (%.2f%% revisits, %.2f%% "
                             "overtime revisits).\n",
                             visitCnt, (100.0 * revisitCnt) / visitCnt, (100.0 * overtimeRevisitCnt) / visitCnt);

                if (ctx->verbose && !netsQueue.empty())
                    log_info("  ripped up %d previously routed nets. continue "
                             "routing.\n",
                             int(netsQueue.size()));
            }

            if (!ctx->verbose)
                log_info("iteration %d: routed %d nets without ripup, routed %d "
                         "nets with ripup.\n",
                         iterCnt, normalRouteCnt, int(ripupQueue.size()));

            totalVisitCnt += visitCnt;
            totalRevisitCnt += revisitCnt;
            totalOvertimeRevisitCnt += overtimeRevisitCnt;

            if (iterCnt == 8 || iterCnt == 16 || iterCnt == 32 || iterCnt == 64 || iterCnt == 128)
                ripup_penalty += ctx->getRipupDelayPenalty();
        }

        log_info("routing complete after %d iterations.\n", iterCnt);

        log_info("visited %d PIPs (%.2f%% revisits, %.2f%% "
                 "overtime revisits).\n",
                 totalVisitCnt, (100.0 * totalRevisitCnt) / totalVisitCnt,
                 (100.0 * totalOvertimeRevisitCnt) / totalVisitCnt);

        log_info("Checksum: 0x%08x\n", ctx->checksum());
#ifndef NDEBUG
        ctx->check();
#endif
        return true;
    } catch (log_execution_error_exception) {
#ifndef NDEBUG
        ctx->check();
#endif
        return false;
    }
}

bool Context::getActualRouteDelay(WireId src_wire, WireId dst_wire, delay_t &delay)
{
    RipupScoreboard scores;
    Router router(this, scores, src_wire, dst_wire);
    if (router.routedOkay)
        delay = router.visited.at(dst_wire).delay;
    return router.routedOkay;
}

NEXTPNR_NAMESPACE_END
