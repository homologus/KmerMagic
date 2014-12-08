/**
>HEADER
    Copyright (c) 2013, 2014 Rob Patro rob.patro@cs.stonybrook.edu

    This file is part of Sailfish.

    Sailfish is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Sailfish is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Sailfish.  If not, see <http://www.gnu.org/licenses/>.
<HEADER
**/


#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <map>
#include <vector>
#include <unordered_set>
#include <mutex>
#include <thread>
#include <sstream>
#include <exception>
#include <random>
#include <queue>
#include <unordered_map>
#include "btree_map.h"
#include "btree_set.h"

// C++ string formatting library
#include "format.h"

// C Includes for BWA
#include <cstdio>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <cctype>

extern "C" {
#include "bwa.h"
#include "bwamem.h"
#include "ksort.h"
#include "kvec.h"
#include "utils.h"
}

// Jellyfish 2 include
#include "jellyfish/mer_dna.hpp"
#include "jellyfish/stream_manager.hpp"
#include "jellyfish/whole_sequence_parser.hpp"

// Boost Includes
#include <boost/filesystem.hpp>
#include <boost/container/flat_map.hpp>
#include <boost/dynamic_bitset/dynamic_bitset.hpp>
#include <boost/range/irange.hpp>
#include <boost/program_options.hpp>
#include <boost/lockfree/queue.hpp>
#include <boost/thread/thread.hpp>

// TBB Includes
#include "tbb/concurrent_unordered_set.h"
#include "tbb/concurrent_vector.h"
#include "tbb/concurrent_unordered_map.h"
#include "tbb/concurrent_queue.h"
#include "tbb/parallel_for.h"
#include "tbb/parallel_for_each.h"
#include "tbb/parallel_reduce.h"
#include "tbb/blocked_range.h"
#include "tbb/task_scheduler_init.h"
#include "tbb/partitioner.h"

// Logger includes
#include "spdlog/spdlog.h"

// Cereal includes
#include "cereal/types/vector.hpp"
#include "cereal/archives/binary.hpp"

#include "concurrentqueue.h"

// Sailfish / Salmon includes
#include "ClusterForest.hpp"
#include "PerfectHashIndex.hpp"
#include "LookUpTableUtils.hpp"
#include "SailfishMath.hpp"
#include "Transcript.hpp"
#include "LibraryFormat.hpp"
#include "SailfishUtils.hpp"
#include "SalmonUtils.hpp"
#include "ReadLibrary.hpp"
#include "SalmonConfig.hpp"

#include "AlignmentGroup.hpp"
#include "PairSequenceParser.hpp"
#include "FragmentLengthDistribution.hpp"
#include "ReadExperiment.hpp"
#include "SalmonOpts.hpp"

/* This allows us to use CLASP for optimal MEM
 * chaining.  However, this seems to be neither
 * computationally efficient, nor significantly
 * better than the greedy chaining, so I'm temporarily
 * removing this un-necessary dependency.  If you
 * (other dev or future Rob) re-instate this in the future
 * remember to re-enable the CLASP fetch and build
 * steps in the CMakeLists.txt files
 *
 *#include "FragmentList.hpp"
 */

extern unsigned char nst_nt4_table[256];
char* bwa_pg = "cha";


/******* STUFF THAT IS STATIC IN BWAMEM THAT WE NEED HERE --- Just re-define it *************/
#define intv_lt(a, b) ((a).info < (b).info)
KSORT_INIT(mem_intv, bwtintv_t, intv_lt)

typedef struct {
    bwtintv_v mem, mem1, *tmpv[2];
} smem_aux_t;

static smem_aux_t *smem_aux_init()
{
    smem_aux_t *a;
    a = static_cast<smem_aux_t*>(calloc(1, sizeof(smem_aux_t)));
    a->tmpv[0] = static_cast<bwtintv_v*>(calloc(1, sizeof(bwtintv_v)));
    a->tmpv[1] = static_cast<bwtintv_v*>(calloc(1, sizeof(bwtintv_v)));
    return a;
}

static void smem_aux_destroy(smem_aux_t *a)
{
    free(a->tmpv[0]->a); free(a->tmpv[0]);
    free(a->tmpv[1]->a); free(a->tmpv[1]);
    free(a->mem.a); free(a->mem1.a);
    free(a);
}

static void mem_collect_intv(const SalmonOpts& sopt, const mem_opt_t *opt, const bwt_t *bwt, int len, const uint8_t *seq, smem_aux_t *a)
{
    int i, k, x = 0, old_n;
    int start_width = (opt->flag & MEM_F_SELF_OVLP)? 2 : 1;
    int split_len = (int)(opt->min_seed_len * opt->split_factor + .499);
    a->mem.n = 0;
    // first pass: find all SMEMs
    while (x < len) {
        if (seq[x] < 4) {
            x = bwt_smem1(bwt, len, seq, x, start_width, &a->mem1, a->tmpv);
            for (i = 0; i < a->mem1.n; ++i) {
                bwtintv_t *p = &a->mem1.a[i];
                int slen = (uint32_t)p->info - (p->info>>32); // seed length
                if (slen >= opt->min_seed_len)
                    kv_push(bwtintv_t, a->mem, *p);
            }
        } else ++x;
    }
    // second pass: find MEMs inside a long SMEM
    old_n = a->mem.n;
    for (k = 0; k < old_n; ++k) {
        bwtintv_t *p = &a->mem.a[k];
        int start = p->info>>32, end = (int32_t)p->info;
        if (end - start < split_len || p->x[2] > opt->split_width) continue;
        bwt_smem1(bwt, len, seq, (start + end)>>1, p->x[2]+1, &a->mem1, a->tmpv);
        for (i = 0; i < a->mem1.n; ++i)
            if ((uint32_t)a->mem1.a[i].info - (a->mem1.a[i].info>>32) >= opt->min_seed_len)
                kv_push(bwtintv_t, a->mem, a->mem1.a[i]);
    }
    // third pass: LAST-like
    if (sopt.extraSeedPass and opt->max_mem_intv > 0) {
        x = 0;
        while (x < len) {
            if (seq[x] < 4) {
                if (1) {
                    bwtintv_t m;
                    x = bwt_seed_strategy1(bwt, len, seq, x, opt->min_seed_len, opt->max_mem_intv, &m);
                    if (m.x[2] > 0) kv_push(bwtintv_t, a->mem, m);
                } else { // for now, we never come to this block which is slower
                    x = bwt_smem1a(bwt, len, seq, x, start_width, opt->max_mem_intv, &a->mem1, a->tmpv);
                    for (i = 0; i < a->mem1.n; ++i)
                        kv_push(bwtintv_t, a->mem, a->mem1.a[i]);
                }
            } else ++x;
        }
    }
    // sort
    // ks_introsort(mem_intv, a->mem.n, a->mem.a);
}


/******* END OF STUFF THAT IS STATIC IN BWAMEM THAT WE NEED HERE --- Just re-define it *************/

using paired_parser = pair_sequence_parser<char**>;
using stream_manager = jellyfish::stream_manager<std::vector<std::string>::const_iterator>;
using single_parser = jellyfish::whole_sequence_parser<stream_manager>;

using TranscriptID = uint32_t;
using TranscriptIDVector = std::vector<TranscriptID>;
using KmerIDMap = std::vector<TranscriptIDVector>;
using my_mer = jellyfish::mer_dna_ns::mer_base_static<uint64_t, 1>;

constexpr uint32_t miniBatchSize{1000};

class SMEMAlignment {
    public:
        SMEMAlignment() :
            transcriptID_(std::numeric_limits<TranscriptID>::max()),
            format_(LibraryFormat::formatFromID(0)),
            score_(0.0),
            fragLength_(0),
            logProb(sailfish::math::LOG_0) {}

        SMEMAlignment(TranscriptID transcriptIDIn, LibraryFormat format,
                  double scoreIn = 0.0, uint32_t fragLengthIn= 0,
                  double logProbIn = sailfish::math::LOG_0) :
            transcriptID_(transcriptIDIn), format_(format), score_(scoreIn),
            fragLength_(fragLengthIn), logProb(logProbIn) {}

        inline TranscriptID transcriptID() { return transcriptID_; }
        inline uint32_t fragLength() { return fragLength_; }
        inline LibraryFormat libFormat() { return format_; }
        inline double score() { return score_; }
        // inline double coverage() {  return static_cast<double>(kmerCount) / fragLength_; };
        uint32_t kmerCount;
        double logProb;

        template <typename Archive>
        void save(Archive& archive) const {
            archive(transcriptID_, format_.formatID(), score_, fragLength_);
        }

        template <typename Archive>
        void load(Archive& archive) {
            uint8_t formatID;
            archive(transcriptID_, formatID, score_, fragLength_);
            format_ = LibraryFormat::formatFromID(formatID);
        }

    private:
        TranscriptID transcriptID_;
        LibraryFormat format_;
        double score_;
        uint32_t fragLength_;
};


struct QueueTraits : public moodycamel::ConcurrentQueueDefaultTraits
{
        static const size_t BLOCK_SIZE = 256;       // Use bigger blocks
};

using AlnGroupQueue = moodycamel::ConcurrentQueue<AlignmentGroup<SMEMAlignment>*>;

inline double logAlignFormatProb(const LibraryFormat observed, const LibraryFormat expected) {

    if (observed.type != expected.type or
        observed.orientation != expected.orientation ) {
        return sailfish::math::LOG_0;
    } else {
        if (expected.strandedness == ReadStrandedness::U) {
            return sailfish::math::LOG_ONEHALF;
        } else {
            if (expected.strandedness == observed.strandedness) {
                return sailfish::math::LOG_1;
            } else {
                return sailfish::math::LOG_0;
            }
        }
    }

    fmt::print(stderr, "WARNING: logAlignFormatProb --- should not get here");
    return sailfish::math::LOG_0;
}

void processMiniBatch(
        double logForgettingMass,
        ReadLibrary& readLib,
        const SalmonOpts& salmonOpts,
        std::vector<AlignmentGroup<SMEMAlignment>*>& batchHits,
        std::vector<Transcript>& transcripts,
        ClusterForest& clusterForest,
        FragmentLengthDistribution& fragLengthDist,
        std::atomic<uint64_t>& numAssignedFragments,
        std::default_random_engine& randEng,
        bool initialRound,
        bool& burnedIn
        ) {

    using sailfish::math::LOG_0;
    using sailfish::math::LOG_1;
    using sailfish::math::LOG_ONEHALF;
    using sailfish::math::logAdd;
    using sailfish::math::logSub;

    constexpr uint64_t numBurninFrags = 5000000;

    size_t numTranscripts{transcripts.size()};
    size_t localNumAssignedFragments{0};
    std::uniform_real_distribution<> uni(0.0, 1.0 + std::numeric_limits<double>::min());
    std::vector<uint64_t> libTypeCounts(LibraryFormat::maxLibTypeID() + 1);

    bool updateCounts = initialRound;

    const auto expectedLibraryFormat = readLib.format();

    // Build reverse map from transcriptID => hit id
    using HitID = uint32_t;
    btree::btree_map<TranscriptID, std::vector<SMEMAlignment*>> hitsForTranscript;
    size_t hitID{0};
    for (auto& hv : batchHits) {
        for (auto& tid : hv->alignments()) {
            hitsForTranscript[tid.transcriptID()].push_back(&tid);
        }
        ++hitID;
    }

    double clustTotal = std::log(batchHits.size()) + logForgettingMass;
    {
        // E-step

        // Iterate over each group of alignments (a group consists of all alignments reported
        // for a single read).  Distribute the read's mass proportionally dependent on the
        // current hits
        for (auto& alnGroup : batchHits) {
            if (alnGroup->size() == 0) { continue; }
            double sumOfAlignProbs{LOG_0};
            // update the cluster-level properties
            bool transcriptUnique{true};
            // AGCHANGE: auto firstTranscriptID = alnGroup.front().transcriptID();
            auto firstTranscriptID = alnGroup->alignments().front().transcriptID();
            std::unordered_set<size_t> observedTranscripts;
            for (auto& aln : alnGroup->alignments()) {
                auto transcriptID = aln.transcriptID();
                auto& transcript = transcripts[transcriptID];
                transcriptUnique = transcriptUnique and (transcriptID == firstTranscriptID);

                double refLength = transcript.RefLength > 0 ? transcript.RefLength : 1.0;
                double coverage = aln.score();
                double logFragProb = (coverage > 0) ? std::log(coverage) : LOG_0;

                // The alignment probability is the product of a transcript-level term (based on abundance and) an alignment-level
                // term below which is P(Q_1) * P(Q_2) * P(F | T)
                double logRefLength = std::log(refLength);
                double transcriptLogCount = transcript.mass();
                if ( transcriptLogCount != LOG_0 ) {
                    double errLike = sailfish::math::LOG_1;
                    if (burnedIn) {
                        //errLike = errMod.logLikelihood(aln, transcript);
                    }

                    double logFragProb = (salmonOpts.useFragLenDist) ?
                                         ((aln.fragLength() > 0) ? fragLengthDist.pmf(static_cast<size_t>(aln.fragLength())) : LOG_1) :
                                         LOG_1;
                    // The probability that the fragments align to the given strands in the
                    // given orientations.
                    double logAlignCompatProb = (salmonOpts.useReadCompat) ?
                                                (logAlignFormatProb(aln.libFormat(), expectedLibraryFormat)) :
                                                LOG_1;
                    // Increment the count of this type of read that we've seen
                    ++libTypeCounts[aln.libFormat().formatID()];

                    //aln.logProb = std::log(aln.kmerCount) + (transcriptLogCount - logRefLength);// + qualProb + errLike;
                    //aln.logProb = std::log(std::pow(aln.kmerCount,2.0)) + (transcriptLogCount - logRefLength);// + qualProb + errLike;
                    aln.logProb = (transcriptLogCount - logRefLength) + logFragProb + logAlignCompatProb;// + qualProb + errLike;

                    sumOfAlignProbs = logAdd(sumOfAlignProbs, aln.logProb);

                    if (observedTranscripts.find(transcriptID) == observedTranscripts.end()) {
                        if (updateCounts) { transcripts[transcriptID].addTotalCount(1); }
                        observedTranscripts.insert(transcriptID);
                    }
                } else {
                    aln.logProb = LOG_0;
                }
            }

            // If this fragment has a zero probability,
            // go to the next one
            if (sumOfAlignProbs == LOG_0) {
                continue;
            } else { // otherwise, count it as assigned
                ++localNumAssignedFragments;
            }

            // normalize the hits
            for (auto& aln : alnGroup->alignments()) {
                aln.logProb -= sumOfAlignProbs;
                auto transcriptID = aln.transcriptID();
                auto& transcript = transcripts[transcriptID];

                double r = uni(randEng);
                if (!burnedIn and r < std::exp(aln.logProb)) {
                    //errMod.update(aln, transcript, aln.logProb, logForgettingMass);
                    double fragLength = aln.fragLength();
                    if (fragLength > 0.0) {
                        //if (aln.fragType() == ReadType::PAIRED_END) {
                        fragLengthDist.addVal(fragLength, logForgettingMass);
                    }
                }


            } // end normalize

            // update the single target transcript
            if (transcriptUnique) {
                if (updateCounts) {
                    transcripts[firstTranscriptID].addUniqueCount(1);
                }
                clusterForest.updateCluster(firstTranscriptID, 1, logForgettingMass, updateCounts);
            } else { // or the appropriate clusters
                clusterForest.mergeClusters<SMEMAlignment>(alnGroup->alignments().begin(), alnGroup->alignments().end());
                clusterForest.updateCluster(alnGroup->alignments().front().transcriptID(), 1, logForgettingMass, updateCounts);
            }

            } // end read group
        }// end timer

        double individualTotal = LOG_0;
        {
            // M-step
            double totalMass{0.0};
            for (auto kv = hitsForTranscript.begin(); kv != hitsForTranscript.end(); ++kv) {
                auto transcriptID = kv->first;
                // The target must be a valid transcript
                if (transcriptID >= numTranscripts or transcriptID < 0) {std::cerr << "index " << transcriptID << " out of bounds\n"; }

                auto& transcript = transcripts[transcriptID];

                // The prior probability
                double hitMass{LOG_0};

                // The set of alignments that match transcriptID
                auto& hits = kv->second;
                std::for_each(hits.begin(), hits.end(), [&](SMEMAlignment* aln) -> void {
                        if (!std::isfinite(aln->logProb)) { std::cerr << "hitMass = " << aln->logProb << "\n"; }
                        hitMass = logAdd(hitMass, aln->logProb);
                        });

                double updateMass = logForgettingMass + hitMass;
                individualTotal = logAdd(individualTotal, updateMass);
                totalMass = logAdd(totalMass, updateMass);
                transcript.addMass(updateMass);
            } // end for
        } // end timer
        numAssignedFragments += localNumAssignedFragments;
        if (numAssignedFragments >= numBurninFrags and !burnedIn) { burnedIn = true; }
        readLib.updateLibTypeCounts(libTypeCounts);
}

uint32_t basesCovered(std::vector<uint32_t>& kmerHits) {
    std::sort(kmerHits.begin(), kmerHits.end());
    uint32_t covered{0};
    uint32_t lastHit{0};
    uint32_t kl{20};
    for (auto h : kmerHits) {
        covered += std::min(h - lastHit, kl);
        lastHit = h;
    }
    return covered;
}

uint32_t basesCovered(std::vector<uint32_t>& posLeft, std::vector<uint32_t>& posRight) {
    return basesCovered(posLeft) + basesCovered(posRight);
}

class KmerVote {
    public:
        KmerVote(int32_t vp, uint32_t rp, uint32_t vl) : votePos(vp), readPos(rp), voteLen(vl) {}
        int32_t votePos{0};
        uint32_t readPos{0};
        uint32_t voteLen{0};
        /*
        std::string str(){
            return "<" + votePos  + ", "  + readPos  + ", "  + voteLen + ">";
        }
        */
};
class MatchFragment {
    public:
        MatchFragment(uint32_t refStart_, uint32_t queryStart_, uint32_t length_) :
            refStart(refStart_), queryStart(queryStart_), length(length_) {}

        uint32_t refStart, queryStart, length;
        uint32_t weight;
        double score;
};

bool precedes(const MatchFragment& a, const MatchFragment& b) {
    return (a.refStart + a.length) < b.refStart and
           (a.queryStart + a.length) < b.queryStart;
}


class TranscriptHitList {
    public:
        int32_t bestHitPos{0};
        uint32_t bestHitCount{0};
        double bestHitScore{0.0};

        std::vector<KmerVote> votes;
        std::vector<KmerVote> rcVotes;

        uint32_t targetID;

        bool isForward_{true};

        void addFragMatch(uint32_t tpos, uint32_t readPos, uint32_t voteLen) {
            int32_t votePos = static_cast<int32_t>(tpos) - readPos;
            votes.emplace_back(votePos, readPos, voteLen);
        }

        void addFragMatchRC(uint32_t tpos, uint32_t readPos, uint32_t voteLen, uint32_t readLen) {
            //int32_t votePos = static_cast<int32_t>(tpos) - (readPos) + voteLen;
            int32_t votePos = static_cast<int32_t>(tpos) - (readLen - readPos);
            rcVotes.emplace_back(votePos, readPos, voteLen);
        }

        uint32_t totalNumHits() { return std::max(votes.size(), rcVotes.size()); }

        bool computeBestLoc_(std::vector<KmerVote>& sVotes, Transcript& transcript,
                             std::string& read, bool isRC,
                             int32_t& maxClusterPos, uint32_t& maxClusterCount, double& maxClusterScore) {
            // Did we update the highest-scoring cluster? This will be set to
            // true iff we have a cluster of a higher score than the score
            // currently given in maxClusterCount.
            bool updatedMaxScore{false};

            if (sVotes.size() == 0) { return updatedMaxScore; }

            struct VoteInfo {
                uint32_t coverage = 0;
                int32_t rightmostBase = 0;
            };

            uint32_t readLen = read.length();

            boost::container::flat_map<uint32_t, VoteInfo> hitMap;
            int32_t currClust{static_cast<int32_t>(sVotes.front().votePos)};
            for (size_t j = 0; j < sVotes.size(); ++j) {

                int32_t votePos = sVotes[j].votePos;
                uint32_t readPos = sVotes[j].readPos;
                uint32_t voteLen = sVotes[j].voteLen;

                if (votePos >= currClust) {
                    if (votePos - currClust > 10) {
                        currClust = votePos;
                    }
                    auto& hmEntry = hitMap[currClust];

                    hmEntry.coverage += std::min(voteLen, (votePos + readPos + voteLen) - hmEntry.rightmostBase);
                    hmEntry.rightmostBase = votePos + readPos + voteLen;
                } else if (votePos < currClust) {
                    std::cerr << "Should not have votePos = " << votePos << " <  currClust = " << currClust << "\n";
                    std::exit(1);
                }

                if (hitMap[currClust].coverage > maxClusterCount) {
                    maxClusterCount = hitMap[currClust].coverage;
                    maxClusterPos = currClust;
                    maxClusterScore = maxClusterCount / static_cast<double>(readLen);
                    updatedMaxScore = true;
                }

            }
            return updatedMaxScore;
        }

        bool computeBestLoc2_(std::vector<KmerVote>& sVotes, uint32_t tlen,
                              int32_t& maxClusterPos, uint32_t& maxClusterCount, double& maxClusterScore) {

            bool updatedMaxScore{false};

            if (sVotes.size() == 0) { return updatedMaxScore; }

            double weights[] = { 1.0, 0.983471453822, 0.935506985032,
                0.860707976425, 0.765928338365, 0.6592406302, 0.548811636094,
                0.441902209585, 0.344153786865, 0.259240260646,
                0.188875602838};

            uint32_t maxGap = 4;
            uint32_t leftmost = (sVotes.front().votePos > maxGap) ? (sVotes.front().votePos - maxGap) : 0;
            uint32_t rightmost = std::min(sVotes.back().votePos + maxGap, tlen);

            uint32_t span = (rightmost - leftmost);
            std::vector<double> probAln(span, 0.0);
            double kwidth = 1.0 / (2.0 * maxGap);

            size_t nvotes = sVotes.size();
            for (size_t j = 0; j < nvotes; ++j) {
                uint32_t votePos = sVotes[j].votePos;
                uint32_t voteLen = sVotes[j].voteLen;

                auto x = j + 1;
                while (x < nvotes and sVotes[x].votePos == votePos) {
                    voteLen += sVotes[x].voteLen;
                    j += 1;
                    x += 1;
                }


                uint32_t dist{0};
                size_t start = (votePos >= maxGap) ? (votePos - maxGap - leftmost) : (votePos - leftmost);
                size_t mid = votePos - leftmost;
                size_t end = std::min(votePos + maxGap - leftmost, rightmost - leftmost);
                for (size_t k = start; k < end; k += 1) {
                    dist = (mid > k) ? mid - k : k - mid;
                    probAln[k] += weights[dist] * voteLen;
                    if (probAln[k] > maxClusterScore) {
                        maxClusterScore = probAln[k];
                        maxClusterPos = k + leftmost;
                        updatedMaxScore = true;
                    }
                }
            }

            return updatedMaxScore;
        }


        inline uint32_t numSampledHits_(Transcript& transcript, std::string& readIn,
                                        int32_t votePos, int32_t posInRead, int32_t voteLen, bool isRC, uint32_t numTries) {


            // The read starts at this position in the transcript (may be negative!)
            int32_t readStart = votePos;
            // The (uncorrected) length of the read
            int32_t readLen = readIn.length();
            // Pointer to the sequence of the read
            const char* read = readIn.c_str();
            // Don't mess around with unsigned arithmetic here
            int32_t tlen = transcript.RefLength;

            // If the read starts before the first base of the transcript,
            // trim off the initial overhang  and correct the other variables
            if (readStart < 0) {
                if (isRC) {
                    uint32_t correction = -readStart;
                    //std::cerr << "readLen = " << readLen << ", posInRead = " << posInRead << ", voteLen = " << voteLen << ", correction = " << correction << "\n";
                    //std::cerr << "tlen = " << tlen << ", votePos = " << votePos << "\n";
                    read += correction;
                    readLen -= correction;
                    posInRead -= correction;
                    readStart = 0;
                } else {
                    uint32_t correction = -readStart;
                    read += correction;
                    readLen -= correction;
                    posInRead -= correction;
                    readStart = 0;
                }
            }
            // If the read hangs off the end of the transcript,
            // shorten its effective length.
            if (readStart + readLen >= tlen) {
                if (isRC) {
                    uint32_t correction = (readStart + readLen) - transcript.RefLength + 1;
                    //std::cerr << "Trimming RC hit: correction = " << correction << "\n";
                    //std::cerr << "untrimmed read : "  << read << "\n";
                    read += correction;
                    readLen -= correction;
                    if (voteLen > readLen) { voteLen = readLen; }
                    posInRead = 0;
                } else {
                    readLen = tlen - (readStart + 1);
                    voteLen = std::max(voteLen, readLen - (posInRead + voteLen));
                }
            }
            // Finally, clip any reverse complement reads starting at 0
            if (isRC) {

                if (voteLen > readStart) {
                    readLen -= (readLen - (posInRead + voteLen));
                }

            }

            // If the read is too short, it's not useful
            if (readLen <= 15) { return 0; }
            // The step between sample centers (given the number of samples we're going to take)
            double step = (readLen - 1) / static_cast<double>(numTries-1);
            // The strand of the transcript from which we'll extract sequence
            auto dir = (isRC) ? sailfish::stringtools::strand::reverse :
                                sailfish::stringtools::strand::forward;

            bool superVerbose{false};

            if (superVerbose) {
                std::stringstream ss;
                ss << "Supposed hit " << (isRC ? "RC" : "") << "\n";
                ss << "info: votePos = " << votePos << ", posInRead = " << posInRead
                    << ", voteLen = " << voteLen << ", readLen = " << readLen
                    << ", tran len = " << tlen << ", step = " << step << "\n";
                if (readStart + readLen > tlen ) {
                    ss << "ERROR!!!\n";
                    std::cerr << "[[" << ss.str() << "]]";
                    std::exit(1);
                }
                ss << "Transcript name = " << transcript.RefName << "\n";
                ss << "T : ";
                try {
                    for ( size_t j = 0; j < readLen; ++j) {
                        if (isRC) {
                            if (j == posInRead) {
                                char red[] = "\x1b[30m";
                                red[3] = '0' + static_cast<char>(fmt::RED);
                                ss << red;
                            }

                            if (j == posInRead + voteLen) {
                                const char RESET_COLOR[] = "\x1b[0m";
                                ss << RESET_COLOR;
                            }
                            ss << transcript.charBaseAt(readStart+readLen-j,dir);
                        } else {
                            if (j == posInRead ) {
                                char red[] = "\x1b[30m";
                                red[3] = '0' + static_cast<char>(fmt::RED);
                                ss << red;
                            }

                            if (j == posInRead + voteLen) {
                                const char RESET_COLOR[] = "\x1b[0m";
                                ss << RESET_COLOR;
                            }

                            ss << transcript.charBaseAt(readStart+j);
                        }
                    }
                    ss << "\n";
                    char red[] = "\x1b[30m";
                    red[3] = '0' + static_cast<char>(fmt::RED);
                    const char RESET_COLOR[] = "\x1b[0m";

                    ss << "R : " << std::string(read, posInRead) << red << std::string(read + posInRead, voteLen) << RESET_COLOR;
                    if (readLen > posInRead + voteLen) { ss << std::string(read + posInRead + voteLen); }
                    ss << "\n\n";
                } catch (std::exception& e) {
                    std::cerr << "EXCEPTION !!!!!! " << e.what() << "\n";
                }
                std::cerr << ss.str() << "\n";
                ss.clear();
            }

            // The index of the current sample within the read
            int32_t readIndex = 0;

            // The number of loci in the subvotes and their
            // offset patternns
            size_t lpos = 3;
            int leftPattern[] = {-4, -2, 0};
            int rightPattern[] = {0, 2, 4};
            int centerPattern[] = {-4, 0, 4};

            // The number of subvote hits we've had
            uint32_t numHits = 0;
            // Take the samples
            for (size_t i  = 0; i < numTries; ++i) {
                // The sample will be centered around this point
                readIndex = static_cast<uint32_t>(std::round(readStart + i * step)) - readStart;

                // The number of successful sub-ovtes we have
                uint32_t subHit = 0;
                // Select the center sub-vote pattern, unless we're near the end of a read
                int* pattern = &centerPattern[0];
                if (readIndex + pattern[0] < 0) {
                    pattern = &rightPattern[0];
                } else if (readIndex + pattern[lpos-1] >= readLen) {
                    pattern = &leftPattern[0];
                }

                // collect the subvotes
                for (size_t j = 0; j < lpos; ++j) {
                    // the pattern offset
                    int offset = pattern[j];
                    // and sample position it implies within the read
                    int readPos = readIndex + offset;

                    if (readStart + readPos >= tlen) {
                        std::cerr  << "offset = " << offset << ", readPos = " << readPos << ", readStart = " << readStart << ", readStart + readPos = " << readStart + readPos << ", tlen = " << transcript.RefLength << "\n";
                    }

                    subHit += (isRC) ?
                        (transcript.charBaseAt(readStart + readLen - readPos, dir) == sailfish::stringtools::charCanon[read[readPos]]) :
                        (transcript.charBaseAt(readStart + readPos               ) == sailfish::stringtools::charCanon[read[readPos]]);
                }
                // if the entire subvote was successful, this is a hit
                numHits += (subHit == lpos);
            }
            // return the number of hits we had
            return numHits;
        }



        bool computeBestLoc3_(std::vector<KmerVote>& sVotes, Transcript& transcript,
                              std::string& read, bool isRC,
                              int32_t& maxClusterPos, uint32_t& maxClusterCount, double& maxClusterScore) {

            bool updatedMaxScore{false};

            if (sVotes.size() == 0) { return updatedMaxScore; }

            struct LocHitCount {
                int32_t loc;
                uint32_t nhits;
            };

            uint32_t numSamp = 15;
            std::vector<LocHitCount> hitCounts;
            size_t nvotes = sVotes.size();
            int32_t prevPos = -std::numeric_limits<int32_t>::max();
            for (size_t j = 0; j < nvotes; ++j) {
                int32_t votePos = sVotes[j].votePos;
                int32_t posInRead = sVotes[j].readPos;
                int32_t voteLen = sVotes[j].voteLen;
                if (prevPos == votePos) { continue; }
                auto numHits = numSampledHits_(transcript, read, votePos, posInRead, voteLen, isRC, numSamp);
                hitCounts.push_back({votePos, numHits});
                prevPos = votePos;
            }

            uint32_t maxGap = 8;
            uint32_t hitIdx = 0;
            uint32_t accumHits = 0;
            int32_t hitLoc = hitCounts[hitIdx].loc;
            while (hitIdx < hitCounts.size()) {
                uint32_t idx2 = hitIdx;
                while (idx2 < hitCounts.size() and std::abs(hitCounts[idx2].loc - hitLoc) <= maxGap) {
                    accumHits += hitCounts[idx2].nhits;
                    ++idx2;
                }

                double score = static_cast<double>(accumHits) / numSamp;
                if (score > maxClusterScore) {
                    maxClusterCount = accumHits;
                    maxClusterScore = score;
                    maxClusterPos = hitCounts[hitIdx].loc;
                    updatedMaxScore = true;
                }
                accumHits = 0;
                ++hitIdx;
                hitLoc = hitCounts[hitIdx].loc;
            }

            return updatedMaxScore;
        }


        bool computeBestChain(Transcript& transcript, std::string& read) {
            std::sort(votes.begin(), votes.end(),
                    [](const KmerVote& v1, const KmerVote& v2) -> bool {
                        if (v1.votePos == v2.votePos) {
                            return v1.readPos < v2.readPos;
                        }
                        return v1.votePos < v2.votePos;
                    });

            std::sort(rcVotes.begin(), rcVotes.end(),
                    [](const KmerVote& v1, const KmerVote& v2) -> bool {
                        if (v1.votePos == v2.votePos) {
                            return v1.readPos < v2.readPos;
                        }
                        return v1.votePos < v2.votePos;
                    });

            int32_t maxClusterPos{0};
            uint32_t maxClusterCount{0};
            double maxClusterScore{0.0};

            // we don't need the return value from the first call
            static_cast<void>(computeBestLoc_(votes, transcript, read, false, maxClusterPos, maxClusterCount, maxClusterScore));
            bool revIsBest = computeBestLoc_(rcVotes, transcript, read, true, maxClusterPos, maxClusterCount, maxClusterScore);
            isForward_ = not revIsBest;

            bestHitPos = maxClusterPos;
            bestHitCount = maxClusterCount;
            bestHitScore = maxClusterScore;
            return true;
        }

        bool isForward() { return isForward_; }

};

template <typename CoverageCalculator>
inline void collectHitsForRead(const bwaidx_t *idx, const bwtintv_v* a, smem_aux_t* auxHits,
                        mem_opt_t* memOptions, const SalmonOpts& salmonOpts, const uint8_t* read, uint32_t readLen,
                        std::unordered_map<uint64_t, CoverageCalculator>& hits) {

    mem_collect_intv(salmonOpts, memOptions, idx->bwt, readLen, read, auxHits);

    // For each MEM
    for (int i = 0; i < auxHits->mem.n; ++i ) {
        // A pointer to the interval of the MEMs occurences
        bwtintv_t* p = &auxHits->mem.a[i];
        // The start and end positions in the query string (i.e. read) of the MEM
        int qstart = p->info>>32;
        uint32_t qend = static_cast<uint32_t>(p->info);
        int step, count, slen = (qend - qstart); // seed length

        int64_t k;
        step = p->x[2] > memOptions->max_occ? p->x[2] / memOptions->max_occ : 1;
        // For every occurrence of the MEM
        for (k = count = 0; k < p->x[2] && count < memOptions->max_occ; k += step, ++count) {
            bwtint_t pos;
            bwtint_t startPos, endPos;
            int len, isRev, isRevStart, isRevEnd, refID, refIDStart, refIDEnd;
            int queryStart = qstart;
            len = slen;
            uint32_t rlen = readLen;

            // Get the position in the reference index of this MEM occurrence
            int64_t refStart = bwt_sa(idx->bwt, p->x[0] + k);

            pos = startPos = bns_depos(idx->bns, refStart, &isRevStart);
            endPos = bns_depos(idx->bns, refStart + slen - 1, &isRevEnd);
            // If we span the forward/reverse boundary, discard the hit
            if (isRevStart != isRevEnd) {
                continue;
            }
            // Otherwise, isRevStart = isRevEnd so just assign isRev = isRevStart
            isRev = isRevStart;

            // If the hit is reversed --- swap the start and end
            if (isRev) {
                if (endPos > startPos) {
                    std::cerr << "DANGER WILL ROBINSON! Hit is supposedly reversed, but startPos = " << startPos << " < endPos = " << endPos << "\n";
                }
                auto temp = startPos;
                startPos = endPos;
                endPos = temp;
            }
            // Get the ID of the reference sequence in which it occurs
            refID = refIDStart = bns_pos2rid(idx->bns, startPos);
            refIDEnd = bns_pos2rid(idx->bns, endPos);

            if (refID < 0) { continue; } // bridging multiple reference sequences or the forward-reverse boundary;

            auto tlen = idx->bns->anns[refID].len;
            // The refence sequence-relative (e.g. transcript-relative) position of the MEM
            long hitLoc = static_cast<long>(isRev ? endPos : startPos) - idx->bns->anns[refID].offset;

            if ((refIDStart != refIDEnd)) {
                // If a seed spans two transcripts

                // If we're not considering splitting such seeds, then
                // just discard this seed and continue.
                if (not salmonOpts.splitSpanningSeeds) { continue; }

                //std::cerr << "Seed spans two transcripts! --- attempting to split: \n";
                if (!isRev) {
                    // If it's going forward, we have a situation like this
                    // packed transcripts: t1 ===========|t2|==========>
                    // hit:                          |==========>

                    // length of hit in t1
                    auto len1 = tlen - hitLoc;
                    // length of hit in t2
                    auto len2 = slen - len1;
                    if (std::max(len1, len2) < memOptions->min_seed_len) { continue; }

                    /** Keeping this here for now in case I need to debug splitting seeds again
                    std::cerr << "\t hit is in the forward direction: ";
                    std::cerr << "t1 part has length " << len1 << ", t2 part has length " << len2 << "\n";
                    */

                    // If the part in t1 is larger then just cut off the rest
                    if (len1 >= len2) {
                        slen = len1;
                        int32_t votePos = static_cast<int32_t>(hitLoc) - queryStart;
                        //std::cerr << "\t\t t1 (of length " << tlen << ") has larger hit --- new hit length = " << len1 << "; starts at pos " << queryStart << " in the read (votePos will be " << votePos << ")\n";
                    } else {
                        // Otherwise, make the hit be in t2.
                        // Because the hit spans the boundary where t2 begins,
                        // the new seed begins matching at position 0 of
                        // transcript t2
                        hitLoc = 0;
                        slen = len2;
                        // The seed originally started at position q, now it starts  len1 characters to the  right of that
                        queryStart += len1;
                        refID = refIDEnd;
                        int32_t votePos = static_cast<int32_t>(hitLoc) - queryStart;
                        tlen = idx->bns->anns[refID].len;
                        //std::cerr << "\t\t t2 (of length " << tlen << ") has larger hit --- new hit length = " << len2 << "; starts at pos " << queryStart << " in the read (votePos will be " << votePos << ")\n";
                    }
                } else {

                    // If it's going in the reverse direction, we have a situation like this
                    // packed transcripts: t1 <===========|t2|<==========
                    // hit:                          X======Y>======Z>
                    // Which means we have
                    // packed transcripts: t1 <===========|t2|<==========
                    // hit:                          <Z=====Y<======X
                    // length of hit in t1

                    auto len2 = endPos - idx->bns->anns[refIDEnd].offset;
                    auto len1 = slen - len2;
                    if (std::max(len1, len2) < memOptions->min_seed_len) { continue; }

                    /** Keeping this here for now in case I need to debug splitting seeds again
                    std::cerr << "\t hit is in the reverse direction: ";
                    std::cerr << "\n\n";
                    std::cerr << "startPos = " << startPos << ", endPos = " << endPos << ", offset[refIDStart] = "
                              <<  idx->bns->anns[refIDStart].offset << ", offset[refIDEnd] = " << idx->bns->anns[refIDEnd].offset << "\n";
                    std::cerr << "\n\n";
                    std::cerr << "t1 part has length " << len1 << ", t2 part has length " << len2 << "\n\n";
                    */

                    if (len1 >= len2) {
                        slen = len1;
                        hitLoc = tlen - len2;
                        queryStart += len2;
                        rlen -= len2;
                        int32_t votePos = static_cast<int32_t>(hitLoc) - (rlen - queryStart);
                        //std::cerr << "\t\t t1 (hitLoc: " << hitLoc << ") (of length " << tlen << ") has larger hit --- new hit length = " << len1 << "; starts at pos " << queryStart << " in the read (votePos will be " << votePos << ")\n";
                    } else {
                        slen = len2;
                        refID = bns_pos2rid(idx->bns, endPos);
                        tlen = idx->bns->anns[refID].len;
                        hitLoc = len2;
                        rlen = hitLoc + queryStart;
                        int32_t votePos = static_cast<int32_t>(hitLoc) - (rlen - queryStart);
                        //std::cerr << "\t\t t2 (of length " << tlen << ") (hitLoc: " << hitLoc << ") has larger hit --- new hit length = " << len2 << "; starts at pos " << queryStart << " in the read (votePos will be " << votePos << ")\n";
                    }
                }

            }

            if (isRev) {
                hits[refID].addFragMatchRC(hitLoc, queryStart , slen, rlen);
            } else {
                hits[refID].addFragMatch(hitLoc, queryStart, slen);
            }
        } // for k
    }
}

inline bool consistentNames(header_sequence_qual& r) {
    return true;
}

bool consistentNames(std::pair<header_sequence_qual, header_sequence_qual>& rp) {
        auto l1 = rp.first.header.length();
        auto l2 = rp.second.header.length();
        char* sptr = static_cast<char*>(memchr(&rp.first.header[0], ' ', l1));

        bool compat = false;
        // If we didn't find a space in the name of read1
        if (sptr == NULL) {
            if (l1 > 1) {
                compat = (l1 == l2);
                compat = compat and (memcmp(&rp.first.header[0], &rp.second.header[0], l1-1) == 0);
                compat = compat and ((rp.first.header[l1-1] == '1' and rp.second.header[l2-1] == '2')
                                or   (rp.first.header[l1-1] == rp.second.header[l2-1]));
            } else {
                compat = (l1 == l2);
                compat = compat and (rp.first.header[0] == rp.second.header[0]);
            }
        } else {
            size_t offset = sptr - (&rp.first.header[0]);

            // If read2 matches read1 up to and including the space
            if (offset + 1 < l2) {
                compat = memcmp(&rp.first.header[0], &rp.second.header[0], offset) == 0;
                // and after the space, read1 and read2 have an identical character or
                // read1 has a '1' and read2 has a '2', then this is a consistent pair.
                compat = compat and ((rp.first.header[offset+1] == rp.second.header[offset+1])
                                or   (rp.first.header[offset+1] == '1' and rp.second.header[offset+1] == '2'));
            } else {
                compat = false;
            }
        }
        return compat;
}


template <typename CoverageCalculator>
void getHitsForFragment(std::pair<header_sequence_qual, header_sequence_qual>& frag,
                        bwaidx_t *idx,
                        smem_i *itr,
                        const bwtintv_v *a,
                        smem_aux_t* auxHits,
                        mem_opt_t* memOptions,
                        const SalmonOpts& salmonOpts,
                        double coverageThresh,
                        AlignmentGroup<SMEMAlignment>& hitList,
                        uint64_t& hitListCount,
                        std::vector<Transcript>& transcripts) {

    std::unordered_map<uint64_t, CoverageCalculator> leftHits;
    std::unordered_map<uint64_t, CoverageCalculator> leftHitsOld;

    std::unordered_map<uint64_t, CoverageCalculator> rightHits;
    std::unordered_map<uint64_t, CoverageCalculator> rightHitsOld;

    uint32_t leftReadLength{0};
    uint32_t rightReadLength{0};

    /**
    * As soon as we can decide on an acceptable way to validate read names,
    * we'll inform the user and quit if we see something inconsistent.  However,
    * we first need a reasonable way to verify potential naming formats from
    * many different sources.
    */
    /*
    if (!consistentNames(frag)) {
        fmt::MemoryWriter errstream;

        errstream << "Inconsistent paired-end reads!\n";
        errstream << "mate1 : " << frag.first.header << "\n";
        errstream << "mate2 : " << frag.second.header << "\n";
        errstream << "Paired-end reads should appear consistently in their respective files.\n";
        errstream << "Please fix the paire-end input before quantifying with salmon; exiting.\n";

        std::cerr << errstream.str();
        std::exit(-1);
    }
    */

    //---------- End 1 ----------------------//
    {
        std::string readStr   = frag.first.seq;
        uint32_t readLen      = frag.first.seq.size();

        leftReadLength = readLen;

        for (int p = 0; p < readLen; ++p) {
            readStr[p] = nst_nt4_table[static_cast<int>(readStr[p])];
        }

        collectHitsForRead(idx, a, auxHits,
                            memOptions,
                            salmonOpts,
                            reinterpret_cast<const uint8_t*>(readStr.c_str()),
                            readLen,
                            leftHits);
    }

    //---------- End 2 ----------------------//
    {
        std::string readStr   = frag.second.seq;
        uint32_t readLen      = frag.second.seq.size();

        rightReadLength = readLen;

        for (int p = 0; p < readLen; ++p) {
            readStr[p] = nst_nt4_table[static_cast<int>(readStr[p])];
        }

        collectHitsForRead(idx, a, auxHits,
                            memOptions,
                            salmonOpts,
                            reinterpret_cast<const uint8_t*>(readStr.c_str()),
                            readLen,
                            rightHits);
     } // end right

    size_t readHits{0};
    auto& alnList = hitList.alignments();
    alnList.clear();

    //std::cerr << "leftHits.size() = " << leftHits.size() << ", leftHitsOld.size() = " << leftHitsOld.size() <<
    //             "rightHits.size() = " << rightHits.size() << ", rightHitsOld.size() = "<< rightHitsOld.size() << "\n";

    double cutoffLeft{ coverageThresh };//* leftReadLength};
    double cutoffRight{ coverageThresh };//* rightReadLength};

    uint64_t leftHitCount{0};
    uint64_t leftHitOldCount{0};
    size_t readHitsOld{0};

    for (auto& tHitList : leftHits) {
        // Coverage score
        Transcript& t = transcripts[tHitList.first];
        tHitList.second.computeBestChain(t, frag.first.seq);
        ++leftHitCount;
    }

    for (auto& tHitList : rightHits) {

        auto it = leftHits.find(tHitList.first);
        // Coverage score
        if (it != leftHits.end() and it->second.bestHitScore >= cutoffLeft) {
            Transcript& t = transcripts[tHitList.first];
            tHitList.second.computeBestChain(t, frag.second.seq);
            if (tHitList.second.bestHitScore < cutoffRight) { continue; }

            auto end1Start = it->second.bestHitPos;
            auto end2Start = tHitList.second.bestHitPos;

            double score = (it->second.bestHitScore + tHitList.second.bestHitScore) * 0.5;
            uint32_t fragLength = std::abs(static_cast<int32_t>(end1Start) -
                                           static_cast<int32_t>(end2Start)) + rightReadLength;

            bool end1IsForward = it->second.isForward();
            bool end2IsForward = tHitList.second.isForward();

            uint32_t end1Pos = (end1IsForward) ? it->second.bestHitPos : it->second.bestHitPos + leftReadLength;
            uint32_t end2Pos = (end2IsForward) ? tHitList.second.bestHitPos : tHitList.second.bestHitPos + rightReadLength;
            auto fmt = salmon::utils::hitType(end1Pos, end1IsForward, end2Pos, end2IsForward);

            alnList.emplace_back(tHitList.first, fmt, score, fragLength);
            ++readHits;
            ++hitListCount;
        }
    }
}

/**
  *   Get hits for single-end fragment
  *
  *
  */
template <typename CoverageCalculator>
void getHitsForFragment(jellyfish::header_sequence_qual& frag,
                        bwaidx_t *idx,
                        smem_i *itr,
                        const bwtintv_v *a,
                        smem_aux_t* auxHits,
                        mem_opt_t* memOptions,
                        const SalmonOpts& salmonOpts,
                        double coverageThresh,
                        AlignmentGroup<SMEMAlignment>& hitList,
                        uint64_t& hitListCount,
                        std::vector<Transcript>& transcripts) {

    uint64_t leftHitCount{0};

    //std::unordered_map<uint64_t, TranscriptHitList> hits;
    std::unordered_map<uint64_t, CoverageCalculator> hits;

    uint32_t readLength{0};

    //---------- get hits ----------------------//
    {
        std::string readStr   = frag.seq;
        uint32_t readLen      = frag.seq.size();

        readLength = readLen;

        for (int p = 0; p < readLen; ++p) {
            readStr[p] = nst_nt4_table[static_cast<int>(readStr[p])];
        }

        char* readPtr = const_cast<char*>(readStr.c_str());

        collectHitsForRead(idx, a, auxHits,
                            memOptions,
                            salmonOpts,
                            reinterpret_cast<const uint8_t*>(readStr.c_str()),
                            readLen,
                            hits);

    }

    size_t readHits{0};
    auto& alnList = hitList.alignments();
    alnList.clear();

    double cutoff{ coverageThresh };//* readLength};
    for (auto& tHitList : hits) {
        // Coverage score
        Transcript& t = transcripts[tHitList.first];
        tHitList.second.computeBestChain(t, frag.seq);
        // DEBUG -- process ALL HITS
        //if (true) {
        if (tHitList.second.bestHitScore >= cutoff) {

            double score = tHitList.second.bestHitScore;
            bool isForward = tHitList.second.isForward();

            auto fmt = salmon::utils::hitType(tHitList.second.bestHitPos, isForward);

            alnList.emplace_back(tHitList.first, fmt, score);
            readHits += score;
            ++hitListCount;
            ++leftHitCount;
        }
    }

}

// To use the parser in the following, we get "jobs" until none is
// available. A job behaves like a pointer to the type
// jellyfish::sequence_list (see whole_sequence_parser.hpp).
template <typename ParserT, typename CoverageCalculator>
void processReadsMEM(ParserT* parser,
               ReadLibrary& rl,
               AlnGroupQueue& structureCache,
               AlnGroupQueue& outputGroups,
               std::atomic<uint64_t>& numObservedFragments,
               std::atomic<uint64_t>& numAssignedFragments,
               std::atomic<uint64_t>& validHits,
               bwaidx_t *idx,
               std::vector<Transcript>& transcripts,
               std::atomic<uint64_t>& batchNum,
               double& logForgettingMass,
               std::mutex& ffMutex,
               ClusterForest& clusterForest,
               FragmentLengthDistribution& fragLengthDist,
               mem_opt_t* memOptions,
               const SalmonOpts& salmonOpts,
               double coverageThresh,
	           std::mutex& iomutex,
               bool initialRound,
               bool& burnedIn,
               volatile bool& writeToCache
               ) {
  uint64_t count_fwd = 0, count_bwd = 0;

  double forgettingFactor{0.65};

  // Seed with a real random value, if available
  std::random_device rd;

  // Create a random uniform distribution
  std::default_random_engine eng(rd());

  std::vector<AlignmentGroup<SMEMAlignment>*> hitLists;
  //std::vector<std::vector<Alignment>> hitLists;
  uint64_t prevObservedFrags{1};
  hitLists.resize(miniBatchSize);

  uint64_t leftHitCount{0};
  uint64_t hitListCount{0};

  // Super-MEM iterator
  smem_i *itr = smem_itr_init(idx->bwt);
  const bwtintv_v *a = nullptr;
  smem_aux_t* auxHits = smem_aux_init();

  auto expectedLibType = rl.format();

  size_t locRead{0};
  while(true) {
    typename ParserT::job j(*parser); // Get a job from the parser: a bunch of read (at most max_read_group)
    if(j.is_empty()) break;           // If got nothing, quit

    hitLists.resize(j->nb_filled);
    //structureCache.try_dequeue_bulk(hitLists.begin() , j->nb_filled);

    for(size_t i = 0; i < j->nb_filled; ++i) { // For all the read in this batch

        //hitLists[i].setRead(&j->data[i]);

        while (!structureCache.try_dequeue(hitLists[i])) {}
        auto& hitList = *(hitLists[i]);

        getHitsForFragment<CoverageCalculator>(j->data[i], idx, itr, a,
                                               auxHits,
                                               memOptions,
                                               salmonOpts,
                                               coverageThresh,
                                               hitList, hitListCount,
                                               transcripts);

        // If the read mapped to > maxReadOccs places, discard it
        if (hitList.size() > salmonOpts.maxReadOccs ) { hitList.alignments().clear(); }
        validHits += hitList.size();
        locRead++;
        ++numObservedFragments;
        if (numObservedFragments % 50000 == 0) {
    	    iomutex.lock();
            const char RESET_COLOR[] = "\x1b[0m";
            char green[] = "\x1b[30m";
            green[3] = '0' + static_cast<char>(fmt::GREEN);
            char red[] = "\x1b[30m";
            red[3] = '0' + static_cast<char>(fmt::RED);
            fmt::print(stderr, "\033[F\r\r{}processed{} {} {}fragments{}\n", green, red, numObservedFragments, green, RESET_COLOR);
            fmt::print(stderr, "hits per frag:  {}", validHits / static_cast<float>(prevObservedFrags));
    	    iomutex.unlock();
        }


    } // end for i < j->nb_filled

    auto oldBatchNum = batchNum++;
    if (oldBatchNum > 1) {
        ffMutex.lock();
        logForgettingMass += forgettingFactor * std::log(static_cast<double>(oldBatchNum-1)) -
        std::log(std::pow(static_cast<double>(oldBatchNum), forgettingFactor) - 1);
        ffMutex.unlock();
    }

    prevObservedFrags = numObservedFragments;
    processMiniBatch(logForgettingMass, rl, salmonOpts, hitLists, transcripts, clusterForest,
                     fragLengthDist, numAssignedFragments, eng, initialRound, burnedIn);
    if (writeToCache) {
        outputGroups.enqueue_bulk(hitLists.begin(), hitLists.size());
    } else {
        structureCache.enqueue_bulk(hitLists.begin(), hitLists.size());
    }
    // At this point, the parser can re-claim the strings
  }
  smem_aux_destroy(auxHits);
  smem_itr_destroy(itr);
}

// To use the parser in the following, we get "jobs" until none is
// available. A job behaves like a pointer to the type
// jellyfish::sequence_list (see whole_sequence_parser.hpp).
void processCachedAlignmentsHelper(
        ReadLibrary& rl,
        AlnGroupQueue& structureCache,
        AlnGroupQueue& alignmentCache,
        std::atomic<uint64_t>& numObservedFragments,
        std::atomic<uint64_t>& numAssignedFragments,
        std::atomic<uint64_t>& validHits,
        std::vector<Transcript>& transcripts,
        std::atomic<uint64_t>& batchNum,
        double& logForgettingMass,
        std::mutex& ffMutex,
        ClusterForest& clusterForest,
        FragmentLengthDistribution& fragLengthDist,
        const SalmonOpts& salmonOpts,
        std::mutex& iomutex,
        bool initialRound,
        bool& cacheExhausted,
        bool& burnedIn
        ) {

    double forgettingFactor{0.65};

    // Seed with a real random value, if available
    std::random_device rd;

    // Create a random uniform distribution
    std::default_random_engine eng(rd());

    std::vector<AlignmentGroup<SMEMAlignment>*> hitLists;

    uint64_t prevObservedFrags{1};
    auto expectedLibType = rl.format();

    uint32_t batchCount{1000};
    uint64_t locRead{0};
    uint64_t locValidHits{0};
    while(!cacheExhausted) {
        uint32_t numConsumed{0};
        hitLists.resize(batchCount);

        auto it = hitLists.begin();
        while (!cacheExhausted and numConsumed < batchCount) {
            uint32_t obtained = alignmentCache.try_dequeue_bulk(it, batchCount - numConsumed);
            numConsumed += obtained;
            it += obtained;
        }
        hitLists.resize(numConsumed);
        for (auto hitList : hitLists) {
            locValidHits += hitList->size();
        }
        validHits += locValidHits;
        locRead += numConsumed;
        uint64_t prevMod = numObservedFragments % 200000;
        numObservedFragments += numConsumed;
        uint64_t newMod = numObservedFragments % 200000;
        if (newMod < prevMod) {
            iomutex.lock();
            const char RESET_COLOR[] = "\x1b[0m";
            char green[] = "\x1b[30m";
            green[3] = '0' + static_cast<char>(fmt::GREEN);
            char red[] = "\x1b[30m";
            red[3] = '0' + static_cast<char>(fmt::RED);
            fmt::print(stderr, "\033[F\r\r{}processed{} {} {}fragments{}\n", green, red, numObservedFragments, green, RESET_COLOR);
            fmt::print(stderr, "hits per frag:  {} / {} = {}", locValidHits, locRead, locValidHits / static_cast<float>(locRead));
            iomutex.unlock();
        }

        auto oldBatchNum = batchNum++;
        if (oldBatchNum > 1) {
            ffMutex.lock();
            logForgettingMass += forgettingFactor * std::log(static_cast<double>(oldBatchNum-1)) -
                std::log(std::pow(static_cast<double>(oldBatchNum), forgettingFactor) - 1);
            ffMutex.unlock();
        }

        processMiniBatch(logForgettingMass, rl, salmonOpts, hitLists, transcripts, clusterForest,
                fragLengthDist, numAssignedFragments, eng, initialRound, burnedIn);

        structureCache.enqueue_bulk(hitLists.begin() , hitLists.size());
        // At this point, the parser can re-claim the strings
    }

}




int performBiasCorrection(boost::filesystem::path featPath,
                          boost::filesystem::path expPath,
                          double estimatedReadLength,
                          double kmersPerRead,
                          uint64_t mappedKmers,
                          uint32_t merLen,
                          boost::filesystem::path outPath,
                          size_t numThreads);

void processCachedAlignments(
        ReadLibrary& rl,
        AlnGroupQueue& structureCache,
        AlnGroupQueue& alignmentCache,
        std::atomic<uint64_t>& numObservedFragments,
        std::atomic<uint64_t>& numAssignedFragments,
        std::vector<Transcript>& transcripts,
        std::atomic<uint64_t>& batchNum,
        double& logForgettingMass,
        std::mutex& ffMutex,
        ClusterForest& clusterForest,
        FragmentLengthDistribution& fragLengthDist,
        const SalmonOpts& salmonOpts,
        std::mutex& ioMutex,
        bool initialRound,
        bool& cacheExhausted,
        bool& burnedIn,
        size_t numQuantThreads) {

        std::atomic<uint64_t> numValidHits{0};

        std::vector<std::thread> quantThreads;
        for (size_t i = 0; i < numQuantThreads; ++i) {
                quantThreads.emplace_back(processCachedAlignmentsHelper,
                        std::ref(rl),
                        std::ref(structureCache),
                        std::ref(alignmentCache),
                        std::ref(numObservedFragments),
                        std::ref(numAssignedFragments),
                        std::ref(numValidHits),
                        std::ref(transcripts),
                        std::ref(batchNum),
                        std::ref(logForgettingMass),
                        std::ref(ffMutex),
                        std::ref(clusterForest),
                        std::ref(fragLengthDist),
                        std::ref(salmonOpts),
                        std::ref(ioMutex),
                        initialRound,
                        std::ref(cacheExhausted),
                        std::ref(burnedIn));

        }
        for (auto& t : quantThreads) { t.join(); }
}

void processReadLibrary(
        ReadLibrary& rl,
        bwaidx_t* idx,
        std::vector<Transcript>& transcripts,
        ClusterForest& clusterForest,
        std::atomic<uint64_t>& numObservedFragments, // total number of reads we've looked at
        std::atomic<uint64_t>& numAssignedFragments, // total number of assigned reads
        std::atomic<uint64_t>& batchNum,
        bool initialRound,
        bool& burnedIn,
        double& logForgettingMass,
        std::mutex& ffMutex,
        FragmentLengthDistribution& fragLengthDist,
        mem_opt_t* memOptions,
        const SalmonOpts& salmonOpts,
        double coverageThresh,
        bool greedyChain,
        std::mutex& iomutex,
        size_t numThreads,
        AlnGroupQueue& structureCache,
        AlnGroupQueue& outputGroups,
        volatile bool& writeToCache) {

            std::vector<std::thread> threads;

            std::atomic<uint64_t> numValidHits{0};
            rl.checkValid();
            // If the read library is paired-end
            // ------ Paired-end --------
            if (rl.format().type == ReadType::PAIRED_END) {
                char* readFiles[] = { const_cast<char*>(rl.mates1().front().c_str()),
                    const_cast<char*>(rl.mates2().front().c_str()) };

                size_t maxReadGroup{miniBatchSize}; // Number of reads in each "job"
                size_t concurrentFile{2}; // Number of files to read simultaneously
                paired_parser parser(4 * numThreads, maxReadGroup, concurrentFile,
                        readFiles, readFiles + 2);

                for(int i = 0; i < numThreads; ++i)  {
                    if (greedyChain) {
                        auto threadFun = [&]() -> void {
                                    processReadsMEM<paired_parser, TranscriptHitList>(
                                    &parser,
                                    rl,
                                    structureCache,
                                    outputGroups,
                                    numObservedFragments,
                                    numAssignedFragments,
                                    numValidHits,
                                    idx,
                                    transcripts,
                                    batchNum,
                                    logForgettingMass,
                                    ffMutex,
                                    clusterForest,
                                    fragLengthDist,
                                    memOptions,
                                    salmonOpts,
                                    coverageThresh,
                                    iomutex,
                                    initialRound,
                                    burnedIn,
                                    writeToCache);
                        };
                        threads.emplace_back(threadFun);
                    } else {
                        /*
                        auto threadFun = [&]() -> void {
                                    processReadsMEM<paired_parser, FragmentList>(
                                    &parser,
                                    rl,
                                    numObservedFragments,
                                    numAssignedFragments,
                                    idx,
                                    transcripts,
                                    batchNum,
                                    logForgettingMass,
                                    ffMutex,
                                    clusterForest,
                                    fragLengthDist,
                                    memOptions,
                                    salmonOpts,
                                    coverageThresh,
                                    iomutex,
                                    initialRound,
                                    burnedIn);
                        };
                        threads.emplace_back(threadFun);
                        */
                    }
                }

                for(int i = 0; i < numThreads; ++i)
                    threads[i].join();

            } // ------ Single-end --------
            else if (rl.format().type == ReadType::SINGLE_END) {

                char* readFiles[] = { const_cast<char*>(rl.unmated().front().c_str()) };
                size_t maxReadGroup{miniBatchSize}; // Number of files to read simultaneously
                size_t concurrentFile{1}; // Number of reads in each "job"
                stream_manager streams( rl.unmated().begin(),
                        rl.unmated().end(), concurrentFile);

                single_parser parser(4 * numThreads, maxReadGroup, concurrentFile,
                        streams);

                for(int i = 0; i < numThreads; ++i)  {
                    if (greedyChain) {
                        auto threadFun = [&]() -> void {
                                    processReadsMEM<single_parser, TranscriptHitList>( &parser,
                                    rl,
                                    structureCache,
                                    outputGroups,
                                    numObservedFragments,
                                    numAssignedFragments,
                                    numValidHits,
                                    idx,
                                    transcripts,
                                    batchNum,
                                    logForgettingMass,
                                    ffMutex,
                                    clusterForest,
                                    fragLengthDist,
                                    memOptions,
                                    salmonOpts,
                                    coverageThresh,
                                    iomutex,
                                    initialRound,
                                    burnedIn,
                                    writeToCache);
                        };
                        threads.emplace_back(threadFun);
                    } else {
                        /*
                        auto threadFun = [&]() -> void {
                                    processReadsMEM<single_parser, FragmentList>( &parser,
                                    rl,
                                    numObservedFragments,
                                    numAssignedFragments,
                                    idx,
                                    transcripts,
                                    batchNum,
                                    logForgettingMass,
                                    ffMutex,
                                    clusterForest,
                                    fragLengthDist,
                                    coverageThresh,
                                    iomutex,
                                    initialRound,
                                    burnedIn);
                        };
                        threads.emplace_back(threadFun);
                        */
                    }
                }
                for(int i = 0; i < numThreads; ++i)
                    threads[i].join();
            } // ------ END Single-end --------
}

bool writeAlignmentCacheToFile(
        AlnGroupQueue& outputGroups,
        AlnGroupQueue& structureCache,
        uint64_t& numWritten,
        std::atomic<uint64_t>& numObservedFragments,
        uint64_t numRequiredFragments,
        volatile bool& writeToCache,
        cereal::BinaryOutputArchive& outputStream ) {

        size_t blockSize{miniBatchSize};
        size_t numDequed{0};
        AlignmentGroup<SMEMAlignment>* alnGroups[blockSize];

        while (writeToCache) {
            while ( (numDequed = outputGroups.try_dequeue_bulk(alnGroups, blockSize)) > 0) {
                for (size_t i = 0; i < numDequed; ++i) {
                    outputStream((*alnGroups[i]));
                    ++numWritten;
                }

                structureCache.enqueue_bulk(alnGroups, numDequed);
                // If, at any point, we've seen the required number of
                // fragments, then we don't need the cache any longer.
                if (numObservedFragments > numRequiredFragments) {
                    writeToCache = false;
                }
            }
        }
        while (outputGroups.try_dequeue(alnGroups[0])) {
            outputStream((*alnGroups[0]));
            ++numWritten;
            structureCache.enqueue(alnGroups[0]);
        }

        return true;
}

bool readAlignmentCache(
        AlnGroupQueue& alnGroupQueue,
        AlnGroupQueue& structureCache,
        uint64_t numWritten,
        bool& finishedParsing,
        std::ifstream& inputStream,
        cereal::BinaryInputArchive& inputArchive) {

        uint64_t numRead{0};
        AlignmentGroup<SMEMAlignment>* alnGroup;
        while (numRead < numWritten) {
            while (!structureCache.try_dequeue(alnGroup)) {}
            inputArchive((*alnGroup));
            alnGroupQueue.enqueue(alnGroup);
            ++numRead;
        }
        finishedParsing = true;
        return true;
}

struct CacheFile {
    CacheFile(boost::filesystem::path& pathIn, uint64_t numWrittenIn) :
        filePath(pathIn), numWritten(numWrittenIn) {}

    boost::filesystem::path filePath;
    uint64_t numWritten{0};
};

/**
  *  Quantify the targets given in the file `transcriptFile` using the
  *  reads in the given set of `readLibraries`, and write the results
  *  to the file `outputFile`.  The reads are assumed to be in the format
  *  specified by `libFmt`.
  *
  */
void quantifyLibrary(
        ReadExperiment& experiment,
        bool greedyChain,
        mem_opt_t* memOptions,
        SalmonOpts& salmonOpts,
        double coverageThresh,
        size_t numRequiredFragments,
        uint32_t numQuantThreads) {

    bool burnedIn{false};
    //ErrorModel errMod(1.00);
    auto& refs = experiment.transcripts();
    size_t numTranscripts = refs.size();
    std::atomic<uint64_t> numObservedFragments{0};

    auto jointLog = spdlog::get("jointLog");

    size_t maxFragLen = 800;
    size_t meanFragLen = 200;
    size_t fragLenStd = 80;
    size_t fragLenKernelN = 4;
    double fragLenKernelP = 0.5;
    FragmentLengthDistribution fragLengthDist(1.0, maxFragLen,
                                              meanFragLen, fragLenStd,
                                              fragLenKernelN,
                                              fragLenKernelP, 1);
    double logForgettingMass{std::log(1.0)};
    double forgettingFactor{0.60};
    bool initialRound{true};

    std::mutex ffMutex;
    std::mutex ioMutex;

    size_t numPrevObservedFragments = 0;
    std::vector<CacheFile> cacheFiles;

    size_t maxReadGroup{miniBatchSize};
    uint32_t structCacheSize = numQuantThreads * maxReadGroup * 10;
    AlnGroupQueue groupCache(structCacheSize);
    for (size_t i = 0; i < structCacheSize; ++i) {
        groupCache.enqueue( new AlignmentGroup<SMEMAlignment>() );
    }

    while (numObservedFragments < numRequiredFragments) {
        if (!initialRound) {
            bool didReset = (salmonOpts.disableMappingCache) ?
                            (experiment.reset()) :
                            (experiment.softReset());

            if (!didReset) {
                std::string errmsg = fmt::sprintf(
                  "\n\n======== WARNING ========\n"
                  "One of the provided read files: [{}] "
                  "is not a regular file and therefore can't be read from "
                  "more than once.\n\n"
                  "We observed only {} mapping fragments when we wanted at least {}.\n\n"
                  "Please consider re-running Salmon with these reads "
                  "as a regular file!\n"
                  "NOTE: If you received this warning from salmon but did not "
                  "disable the mapping cache (--disableMappingCache), then there \n"
                  "was some other problem. Please make sure, e.g., that you have not "
                  "run out of disk space.\n"
                  "==========================\n\n",
                  experiment.readFilesAsString(), numObservedFragments, numRequiredFragments);
                jointLog->warn(errmsg);
                break;
            }
            numPrevObservedFragments = numObservedFragments;
        }

        if (initialRound or salmonOpts.disableMappingCache) {
            AlnGroupQueue outputGroups(structCacheSize);

            volatile bool writeToCache = !salmonOpts.disableMappingCache;
            auto processReadLibraryCallback =  [&](
                    ReadLibrary& rl, bwaidx_t* idx,
                    std::vector<Transcript>& transcripts, ClusterForest& clusterForest,
                    std::atomic<uint64_t>& numAssignedFragments,
                    std::atomic<uint64_t>& batchNum, size_t numQuantThreads,
                    bool& burnedIn) -> void  {

                // The file where the alignment cache was / will be written
                fmt::MemoryWriter fname;
                fname << "alnCache_" << cacheFiles.size() << ".bin";
                boost::filesystem::path alnCacheFilename = salmonOpts.outputDirectory / fname.str();
                cacheFiles.emplace_back(alnCacheFilename, uint64_t(0));

                std::unique_ptr<std::ofstream> alnCacheFile{nullptr};
                std::unique_ptr<std::thread> cacheWriterThread{nullptr};
                if (writeToCache) {
                    alnCacheFile.reset(new std::ofstream(alnCacheFilename.c_str(), std::ios::binary));
                    cereal::BinaryOutputArchive alnCacheArchive(*alnCacheFile);
                    cacheWriterThread.reset(new std::thread(writeAlignmentCacheToFile,
                        std::ref(outputGroups),
                        std::ref(groupCache),
                        std::ref(cacheFiles.back().numWritten),
                        std::ref(numObservedFragments),
                        numRequiredFragments,
                        std::ref(writeToCache),
                        std::ref(alnCacheArchive)));
                }

                processReadLibrary(rl, idx, transcripts, clusterForest,
                        numObservedFragments, numAssignedFragments, batchNum,
                        initialRound, burnedIn, logForgettingMass, ffMutex, fragLengthDist,
                        memOptions, salmonOpts, coverageThresh, greedyChain,
                        ioMutex, numQuantThreads,
                        groupCache, outputGroups, writeToCache);

                // join the thread the writes the file
                writeToCache = false;
                if (cacheWriterThread) { cacheWriterThread->join(); }
                if (alnCacheFile) { alnCacheFile->close(); }
            };

            // Process all of the reads
            experiment.processReads(numQuantThreads, processReadLibraryCallback);
        } else {
            AlnGroupQueue alnGroupQueue;


            uint32_t libNum{0};
            auto processReadLibraryCallback =  [&](
                    ReadLibrary& rl, bwaidx_t* idx,
                    std::vector<Transcript>& transcripts, ClusterForest& clusterForest,
                    std::atomic<uint64_t>& numAssignedFragments,
                    std::atomic<uint64_t>& batchNum, size_t numQuantThreads,
                    bool& burnedIn) -> void  {

                bool finishedParsing{false};

                // The file where the alignment cache was / will be written
                auto& cf = cacheFiles[libNum];
                ++libNum;

                std::ifstream alnCacheFile(cf.filePath.c_str(), std::ios::binary);
                cereal::BinaryInputArchive alnCacheArchive(alnCacheFile);

                std::thread cacheReaderThread(readAlignmentCache,
                        std::ref(alnGroupQueue),
                        std::ref(groupCache),
                        cf.numWritten,
                        std::ref(finishedParsing),
                        std::ref(alnCacheFile),
                        std::ref(alnCacheArchive));

                processCachedAlignments(rl, groupCache, alnGroupQueue,
                        numObservedFragments, numAssignedFragments,
                        transcripts, batchNum, logForgettingMass,
                        ffMutex, clusterForest, fragLengthDist,
                        salmonOpts, ioMutex, initialRound, finishedParsing,
                        burnedIn, numQuantThreads);

                cacheReaderThread.join();
                alnCacheFile.close();
            };

            // Process all of the reads
            experiment.processReads(numQuantThreads, processReadLibraryCallback);
        }

        initialRound = false;
        fmt::print(stderr, "\n# observed = {} / # required = {}\n",
                   numObservedFragments, numRequiredFragments);
        fmt::print(stderr, "# assigned = {} / # observed (this round) = {}\033[F\033[F",
                   experiment.numAssignedFragments(),
                   numObservedFragments - numPrevObservedFragments);
    }
    fmt::print(stderr, "\n\n\n\n");

    AlignmentGroup<SMEMAlignment>* ag;
    while (groupCache.try_dequeue(ag)) { delete ag; }
    // delete any temporary alignment cache files
    for (auto& cf : cacheFiles) {
        if (boost::filesystem::exists(cf.filePath)) {
            boost::filesystem::remove(cf.filePath);
        }
    }

    jointLog->info("finished quantifyLibrary()\n");
}

int performBiasCorrectionSalmon(
        boost::filesystem::path featureFile,
        boost::filesystem::path expressionFile,
        boost::filesystem::path outputFile,
        size_t numThreads);

int salmonQuantify(int argc, char *argv[]) {
    using std::cerr;
    using std::vector;
    using std::string;
    namespace bfs = boost::filesystem;
    namespace po = boost::program_options;

    bool biasCorrect{false};
    bool optChain{false};
    uint32_t maxThreads = std::thread::hardware_concurrency();
    size_t requiredObservations;

    SalmonOpts sopt;
    mem_opt_t* memOptions = mem_opt_init();
    memOptions->split_factor = 1.5;

    double coverageThresh;
    vector<string> unmatedReadFiles;
    vector<string> mate1ReadFiles;
    vector<string> mate2ReadFiles;

    po::options_description generic("salmon quant options");
    generic.add_options()
    ("version,v", "print version string")
    ("help,h", "produce help message")
    ("index,i", po::value<string>()->required(), "Salmon index")
    ("libtype,l", po::value<std::string>()->required(), "Format string describing the library type")
    ("unmated_reads,r", po::value<vector<string>>(&unmatedReadFiles)->multitoken(),
     "List of files containing unmated reads of (e.g. single-end reads)")
    ("mates1,1", po::value<vector<string>>(&mate1ReadFiles)->multitoken(),
        "File containing the #1 mates")
    ("mates2,2", po::value<vector<string>>(&mate2ReadFiles)->multitoken(),
        "File containing the #2 mates")
    ("threads,p", po::value<uint32_t>()->default_value(maxThreads), "The number of threads to use concurrently.")
    ("useReadCompat,e", po::bool_switch(&(sopt.useReadCompat))->default_value(false), "[Currently Experimental] : "
                        "Use the orientation in which fragments were \"mapped\"  to assign them a probability.  For "
                        "example, fragments with an incorrect relative oritenation with respect  to the provided library "
                        "format string will be assigned a 0 probability.")
    ("useFragLenDist,d", po::bool_switch(&(sopt.useFragLenDist))->default_value(false), "[Currently Experimental] : "
                        "Consider concordance with the learned fragment length distribution when trying to determing "
                        "the probability that a fragment has originated from a specified location.  Fragments with "
                        "unlikely lengths will be assigned a smaller relative probability than those with more likely "
                        "lengths.")
    ("num_required_obs,n", po::value(&requiredObservations)->default_value(50000000),
                                        "The minimum number of observations (mapped reads) that must be observed before "
                                        "the inference procedure will terminate.  If fewer mapped reads exist in the "
                                        "input file, then it will be read through multiple times.")
    ("minLen,k", po::value<int>(&(memOptions->min_seed_len))->default_value(19), "(S)MEMs smaller than this size won't be considered.")
    ("maxOcc,m", po::value<int>(&(memOptions->max_occ))->default_value(200), "(S)MEMs occuring more than this many times won't be considered.")
    ("maxReadOcc,w", po::value<uint32_t>(&(sopt.maxReadOccs))->default_value(100), "Reads \"mapping\" to more than this many places won't be considered.")
    ("splitWidth,s", po::value<int>(&(memOptions->split_width))->default_value(0), "If (S)MEM occurs fewer than this many times, search for smaller, contained MEMs. "
                                        "The default value will not split (S)MEMs, a higher value will result in more MEMs being explore and, thus, will "
                                        "result in increased running time.")
    ("splitSpanningSeeds,b", po::bool_switch(&(sopt.splitSpanningSeeds))->default_value(false), "Attempt to split seeds that happen to fall on the "
                                        "boundary between two transcripts.  This can improve the  fragment hit-rate, but is usually not necessary.")
    ("disableMappingCache", po::bool_switch(&(sopt.disableMappingCache))->default_value(false), "Setting this option disables the creation and use "
                                        "of the \"mapping cache\" file.  The mapping cache can speed up quantification significantly for smaller read "
                                        "libraries (i.e. where the number of mapped fragments is less than the required number of observations). However, "
                                        "for very large read libraries, the mapping cache is unnecessary, and disabling it may allow salmon to more effectively "
                                        "make use of a very large number of threads.")
    ("extraSensitive", po::bool_switch(&(sopt.extraSeedPass))->default_value(false), "Setting this option enables an extra pass of \"seed\" search. "
                                        "Enabling this option may improve sensitivity (the number of reads having sufficient coverage), but will "
                                        "typically slow down quantification by ~40%.  Consider enabling this option if you find the mapping rate to "
                                        "be significantly lower than expected.")
    ("coverage,c", po::value<double>(&coverageThresh)->default_value(0.75), "required coverage of read by union of SMEMs to consider it a \"hit\".")
    ("output,o", po::value<std::string>()->required(), "Output quantification file.")
    ("bias_correct", po::value(&biasCorrect)->zero_tokens(), "[Experimental: Output both bias-corrected and non-bias-corrected "
                                                               "qunatification estimates.")
    ("gene_map,g", po::value<string>(), "File containing a mapping of transcripts to genes.  If this file is provided "
                                        "Salmon will output both quant.sf and quant.genes.sf files, where the latter "
                                        "contains aggregated gene-level abundance estimates.  The transcript to gene mapping "
                                        "should be provided as either a GTF file, or a in a simple tab-delimited format "
                                        "where each line contains the name of a transcript and the gene to which it belongs "
                                        "separated by a tab.  The extension of the file is used to determine how the file "
                                        "should be parsed.  Files ending in \'.gtf\' or \'.gff\' are assumed to be in GTF "
                                        "format; files with any other extension are assumed to be in the simple format.");
    //("optChain", po::bool_switch(&optChain)->default_value(false), "Chain MEMs optimally rather than greedily")

    po::variables_map vm;
    try {
        auto orderedOptions = po::command_line_parser(argc,argv).
            options(generic).run();

        po::store(orderedOptions, vm);

        if ( vm.count("help") ) {
            auto hstring = R"(
Quant
==========
Perform streaming SMEM-based estimation of
transcript abundance from RNA-seq reads
)";
            std::cout << hstring << std::endl;
            std::cout << generic << std::endl;
            std::exit(1);
        }

        po::notify(vm);

        std::stringstream commentStream;
        commentStream << "# salmon (smem-based) v" << salmon::version << "\n";
        commentStream << "# [ program ] => salmon \n";
        commentStream << "# [ command ] => quant \n";
        for (auto& opt : orderedOptions.options) {
            commentStream << "# [ " << opt.string_key << " ] => {";
            for (auto& val : opt.value) {
                commentStream << " " << val;
            }
            commentStream << " }\n";
        }
        std::string commentString = commentStream.str();
        fmt::print(stderr, "{}", commentString);

        // Verify the gene_map before we start doing any real work.
        bfs::path geneMapPath;
        if (vm.count("gene_map")) {
            // Make sure the provided file exists
            geneMapPath = vm["gene_map"].as<std::string>();
            if (!bfs::exists(geneMapPath)) {
                std::cerr << "Could not fine transcript <=> gene map file " << geneMapPath << "\n";
                std::cerr << "Exiting now: please either omit the \'gene_map\' option or provide a valid file\n";
                std::exit(1);
            }
        }

        bool greedyChain = !optChain;
        bfs::path outputDirectory(vm["output"].as<std::string>());
        bfs::create_directory(outputDirectory);
        if (!(bfs::exists(outputDirectory) and bfs::is_directory(outputDirectory))) {
            std::cerr << "Couldn't create output directory " << outputDirectory << "\n";
            std::cerr << "exiting\n";
            std::exit(1);
        }

        bfs::path indexDirectory(vm["index"].as<string>());
        bfs::path logDirectory = outputDirectory / "logs";

        sopt.indexDirectory = indexDirectory;
        sopt.outputDirectory = outputDirectory;

        // Create the logger and the logging directory
        bfs::create_directory(logDirectory);
        if (!(bfs::exists(logDirectory) and bfs::is_directory(logDirectory))) {
            std::cerr << "Couldn't create log directory " << logDirectory << "\n";
            std::cerr << "exiting\n";
            std::exit(1);
        }
        std::cerr << "Logs will be written to " << logDirectory.string() << "\n";

        bfs::path logPath = logDirectory / "salmon_quant.log";
        size_t max_q_size = 1000000;
        spdlog::set_async_mode(max_q_size);

        auto fileSink = std::make_shared<spdlog::sinks::simple_file_sink_mt>(logPath.string(), true);
        auto consoleSink = std::make_shared<spdlog::sinks::stderr_sink_mt>();
        auto consoleLog = spdlog::create("consoleLog", {consoleSink});
        auto fileLog = spdlog::create("fileLog", {fileSink});
        auto jointLog = spdlog::create("jointLog", {fileSink, consoleSink});

        jointLog->info() << "parsing read library format";

        vector<ReadLibrary> readLibraries = sailfish::utils::extractReadLibraries(orderedOptions);
        ReadExperiment experiment(readLibraries, indexDirectory);
        uint32_t nbThreads = vm["threads"].as<uint32_t>();

        quantifyLibrary(experiment, greedyChain, memOptions, sopt, coverageThresh,
                        requiredObservations, nbThreads);

        free(memOptions);
        size_t tnum{0};

        jointLog->info("writing output \n");

        bfs::path estFilePath = outputDirectory / "quant.sf";
        salmon::utils::writeAbundances(experiment, estFilePath, commentString);

        bfs::path libCountFilePath = outputDirectory / "libFormatCounts.txt";
        experiment.summarizeLibraryTypeCounts(libCountFilePath);

        if (biasCorrect) {
            auto origExpressionFile = estFilePath;

            auto outputDirectory = estFilePath;
            outputDirectory.remove_filename();

            auto biasFeatPath = indexDirectory / "bias_feats.txt";
            auto biasCorrectedFile = outputDirectory / "quant_bias_corrected.sf";
            performBiasCorrectionSalmon(biasFeatPath, estFilePath, biasCorrectedFile, maxThreads);
        }

        /** If the user requested gene-level abundances, then compute those now **/
        if (vm.count("gene_map")) {
            try {
                sailfish::utils::generateGeneLevelEstimates(geneMapPath,
                                                            outputDirectory,
                                                            biasCorrect);
            } catch (std::invalid_argument& e) {
                fmt::print(stderr, "Error: [{}] when trying to compute gene-level "\
                                   "estimates. The gene-level file(s) may not exist",
                                   e.what());
            }
        }

    } catch (po::error &e) {
        std::cerr << "Exception : [" << e.what() << "]. Exiting.\n";
        std::exit(1);
    } catch (const spdlog::spdlog_ex& ex) {
        std::cerr << "logger failed with : [" << ex.what() << "]. Exiting.\n";
        std::exit(1);
    } catch (std::exception& e) {
        std::cerr << "Exception : [" << e.what() << "]\n";
        std::cerr << argv[0] << " quant was invoked improperly.\n";
        std::cerr << "For usage information, try " << argv[0] << " quant --help\nExiting.\n";
        std::exit(1);
    }


    return 0;
}

