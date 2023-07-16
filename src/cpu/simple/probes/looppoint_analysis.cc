/*
 * Copyright (c) 2023 The Regents of the University of California.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "cpu/simple/probes/looppoint_analysis.hh"

namespace gem5
{

LooppointAnalysis::LooppointAnalysis(const LooppointAnalysisParams &p)
    : ProbeListenerObject(p),
    manager(p.ptmanager),
    BBvalidAddrRange(
        AddrRange(p.bbValidAddrRange.start(),p.bbValidAddrRange.end())
    ),
    markerValidAddrRange(
        AddrRange(p.pcCountPairValidAddrRange.start(),
        p.pcCountPairValidAddrRange.end())),
    startListeningAtInit(p.startListeningAtStart),
    localInstCounter(0),
    filteredKernelInstCount(0),
    fileredUserInstCount(0),
    ifFilterKernelInst(p.startKernelFilterAtStart),
    BBInstCounter(0)

{
    for (int i = 0; i < p.excludeAddrRange.size(); i++)
    {
        BBexcludedAddrRanges.push_back(
            AddrRange(
                p.excludeAddrRange[i].start(),
                p.excludeAddrRange[i].end()
            )
        );
        DPRINTF(
            LooppointAnalysis,
            "added  BBexcludedAddrRanges = (%li,%li)\n",
            BBexcludedAddrRanges.back().start(),
            BBexcludedAddrRanges.back().end()
        );
    }
    DPRINTF(LooppointAnalysis,
        "%i exclued addr ranges\n",
        BBexcludedAddrRanges.size());
    DPRINTF(LooppointAnalysis,
        "new  BBvalidAddrRange = (%li,%li)\n",
        BBvalidAddrRange.start(),
        BBvalidAddrRange.end());
    DPRINTF(LooppointAnalysis,
        "new  markerValidAddrRange = (%li,%li)\n",
        markerValidAddrRange.start(),
        markerValidAddrRange.end());

}

void
LooppointAnalysis::regProbeListeners()
{
    if (startListeningAtInit)
    {
        listeners.push_back(new LooppointAnalysisListener(this, "Commit",
                                                &LooppointAnalysis::checkPc));
        DPRINTF(LooppointAnalysis, "Start listening to the core\n");
    }
}

void
LooppointAnalysis::startListening()
{
    if (listeners.empty())
    {
        listeners.push_back(new LooppointAnalysisListener(this, "Commit",
                                                &LooppointAnalysis::checkPc));
    }
    DPRINTF(LooppointAnalysis, "Current size of listener: %i\n",
                                                    listeners.size());
}

void
LooppointAnalysis::stopListening()
{
    for (auto l = listeners.begin(); l != listeners.end(); ++l) {
        delete (*l);
    }
    listeners.clear();
    DPRINTF(LooppointAnalysis, "Current size of listener: %i\n",
                                                    listeners.size());
}

void
LooppointAnalysis::updateMostRecentPcCount(Addr npc) {
    int count;
    count=manager->getPcCount(npc);
    if (count==-1){
        count = 1;
    }
    PcCountPair pair = PcCountPair(npc,count);
    auto it = std::find_if(localMostRecentPcCount.begin(),
                                localMostRecentPcCount.end(),
                                [&pair](const std::pair<PcCountPair,Tick>& p)
                                { return p.first.getPC()==pair.getPC();});
    if (it == localMostRecentPcCount.end()) {
        while (localMostRecentPcCount.size() >= 5) {
            localMostRecentPcCount.pop_back();
        }
        localMostRecentPcCount.push_front(
                            std::make_pair(pair, curTick()));
    } else {
        if (it != localMostRecentPcCount.begin()) {
            localMostRecentPcCount.push_front(*it);
            localMostRecentPcCount.erase(it);
        }
        it->first = pair;
        it->second = curTick();
    }
}

void
LooppointAnalysis::checkPc(const std::pair<SimpleThread*, StaticInstPtr>& p) {
    SimpleThread* thread = p.first;
    const StaticInstPtr &inst = p.second;
    auto &pcstate =
                thread->getTC()->pcState().as<GenericISA::PCStateWithNext>();

    if (inst->isMicroop() && !inst->isLastMicroop())
        return;

    if (ifFilterKernelInst && !thread->getIsaPtr()->inUserMode()) {
        filteredKernelInstCount ++;
        return;
    }

    if (BBvalidAddrRange.end()>0 && (pcstate.pc() < BBvalidAddrRange.start() ||
        pcstate.pc() > BBvalidAddrRange.end()))
    {
        // if the current instruction is not inside the Basic Block valid
        // address range, then ignore the current instruction
        fileredUserInstCount ++;
        return;
    }

    if (BBexcludedAddrRanges.size()>0) {
        for (int i = 0; i < BBexcludedAddrRanges.size(); i++) {
            if (pcstate.pc() >= BBexcludedAddrRanges[i].start() &&
                pcstate.pc() <= BBexcludedAddrRanges[i].end()) {
                // if the current instruction is inside the Basic Block
                // excluded address range then ignore the current instruction
                fileredUserInstCount ++;
                return;
            }
        }
    }

    if (!BBInstCounter) {
        BBstart = pcstate.pc();
    }

    localInstCounter ++;
    BBInstCounter ++;

    if (inst->isControl()) {
        if (BBfreq.find(BBstart)!=BBfreq.end()) {
            ++BBfreq.find(BBstart)->second;
        } else {
            BBfreq.insert(std::make_pair(BBstart, 1));
        }
        if (encountered_PC.find(BBstart)==encountered_PC.end()) {
            encountered_PC.insert(BBstart);
            manager->updateBBinst(BBstart, BBInstCounter);
        }
        BBInstCounter = 0;

        if (markerValidAddrRange.end()>0
        && (pcstate.pc() < markerValidAddrRange.start()
                            || pcstate.pc() > markerValidAddrRange.end()))
        {
            return;
        }

        if (inst->isDirectCtrl()) {
            if (pcstate.npc() < pcstate.pc()) {
                updateMostRecentPcCount(pcstate.npc());
                manager->countPc(pcstate.npc(), localInstCounter);
                localInstCounter = 0;
            }
        }

    }


}

LooppointAnalysisManager::LooppointAnalysisManager(
    const LooppointAnalysisManagerParams &p)
    : SimObject(p),
    regionLength(p.regionLen),
    globalInstCounter(0),
    mostRecentPC(0)
{
    DPRINTF(LooppointAnalysis, "The region length is %i\n", regionLength);
}

void
LooppointAnalysisManager::countPc(Addr pc, int instCount)
{
    if (counter.find(pc) == counter.end()){
        // If the PC is not in the counter
        // then we insert it with a count of 1
        counter.insert(std::make_pair(pc,1));
    }
    else{
        ++counter.find(pc)->second;
    }

    mostRecentPC = pc;

    globalInstCounter += instCount;
    if (globalInstCounter >= regionLength) {
        exitSimLoopNow(
            "simpoint starting point found");
        // using exitSimLoop instead of exitSimLoopNow because using switch
        // process from KVM to ATOMIC will raise timing error such as
        // when < getCurTick()
        // If not using KVM, it can use exitSimLoopNow to exit with high
        // proirity
        // Using exitSimLoop with curTick happens to generate more exit events
        // than it should be because it doesn't exit the simulation at the
        // moment, so some calls of countPc() goes through before the Python
        // script clear the globalInstCounter
    }
}


void
LooppointAnalysisManager::updateBBinst(Addr BBstart, int inst)
{
    if (BBinst.find(BBstart)==BBinst.end()) {
        BBinst.insert(std::make_pair(BBstart,inst));
    }
}

}
