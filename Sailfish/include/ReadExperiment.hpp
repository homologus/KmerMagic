#ifndef EXPERIMENT_HPP
#define EXPERIMENT_HPP

extern "C" {
#include "bwa.h"
#include "bwamem.h"
#include "kvec.h"
#include "utils.h"
}

// Our includes
#include "ClusterForest.hpp"
#include "Transcript.hpp"
#include "ReadLibrary.hpp"

// Logger includes
#include "spdlog/spdlog.h"

// Boost includes
#include <boost/filesystem.hpp>
#include <boost/range/irange.hpp>

// Standard includes
#include <vector>
#include <memory>

/**
  *  This class represents a library of alignments used to quantify
  *  a set of target transcripts.  The AlignmentLibrary contains info
  *  about both the alignment file and the target sequence (transcripts).
  *  It is used to group them together and track information about them
  *  during the quantification procedure.
  */
class ReadExperiment {

    public:

    ReadExperiment(std::vector<ReadLibrary>& readLibraries,
                   //const boost::filesystem::path& transcriptFile,
                   const boost::filesystem::path& indexDirectory) :
        readLibraries_(readLibraries),
        //transcriptFile_(transcriptFile),
        transcripts_(std::vector<Transcript>()),
        batchNum_(0),
        totalAssignedFragments_(0) {
            namespace bfs = boost::filesystem;

            // Make sure the read libraries are valid.
            for (auto& rl : readLibraries_) { rl.checkValid(); }

            // Make sure the transcript file exists.
            /*
            if (!bfs::exists(transcriptFile_)) {
                std::stringstream ss;
                ss << "The provided transcript file: " << transcriptFile_ <<
                    " does not exist!\n";
                throw std::invalid_argument(ss.str());
            }
            */

            // ====== Load the transcripts from file
            { // mem-based
                bfs::path indexPath = indexDirectory / "bwaidx";
                if ((idx_ = bwa_idx_load(indexPath.string().c_str(), BWA_IDX_BWT|BWA_IDX_BNS|BWA_IDX_PAC)) == 0) {
                    fmt::print(stderr, "Couldn't open index [{}] --- ", indexPath);
                    fmt::print(stderr, "Please make sure that 'salmon index' has been run successfully\n");
                    std::exit(1);
                }
            }

            size_t numRecords = idx_->bns->n_seqs;
            std::vector<Transcript> transcripts_tmp;

            fmt::print(stderr, "Index contained {} targets\n", numRecords);
            //transcripts_.resize(numRecords);
            for (auto i : boost::irange(size_t(0), numRecords)) {
                uint32_t id = i;
                char* name = idx_->bns->anns[i].name;
                uint32_t len = idx_->bns->anns[i].len;
                // copy over the length, then we're done.
                transcripts_tmp.emplace_back(id, name, len);
            }

            std::sort(transcripts_tmp.begin(), transcripts_tmp.end(),
                    [](const Transcript& t1, const Transcript& t2) -> bool {
                    return t1.id < t2.id;
                    });
            double alpha = 0.005;
            char nucTab[256];
            nucTab[0] = 'A'; nucTab[1] = 'C'; nucTab[2] = 'G'; nucTab[3] = 'T';
            for (size_t i = 4; i < 256; ++i) { nucTab[i] = 'N'; }

            // Load the transcript sequence from file
            for (auto& t : transcripts_tmp) {
                transcripts_.emplace_back(t.id, t.RefName.c_str(), t.RefLength, alpha);
                /* from BWA */
                uint8_t* rseq = nullptr;
                int64_t tstart, tend, compLen, l_pac = idx_->bns->l_pac;
                tstart  = idx_->bns->anns[t.id].offset;
                tend = tstart + t.RefLength;
                rseq = bns_get_seq(l_pac, idx_->pac, tstart, tend, &compLen);
                if (compLen != t.RefLength) {
                    fmt::print(stderr,
                               "For transcript {}, stored length ({}) != computed length ({}) --- index may be corrupt. exiting\n",
                               t.RefName, compLen, t.RefLength);
                    std::exit(1);
                }
                std::string seq(t.RefLength, ' ');
                if (rseq != 0) {
                    for (size_t i = 0; i < compLen; ++i) { seq[i] = nucTab[rseq[i]]; }
                }
                transcripts_.back().Sequence = sailfish::stringtools::encodeSequenceInSAM(seq.c_str(), t.RefLength);
                /*
                std::cerr << "TS = " << t.RefName << " : \n";
                std::cerr << seq << "\n VS \n";
                for (size_t i = 0; i < t.RefLength; ++i) {
                    std::cerr << transcripts_.back().charBaseAt(i);
                }
                std::cerr << "\n\n";
                */
                free(rseq);
                /* end BWA code */
            }
            // Since we have the de-coded reference sequences, we no longer need
            // the encoded sequences, so free them.
            free(idx_->pac); idx_->pac = nullptr;
            transcripts_tmp.clear();
            // ====== Done loading the transcripts from file

            // Create the cluster forest for this set of transcripts
            clusters_.reset(new ClusterForest(transcripts_.size(), transcripts_));
        }

    std::vector<Transcript>& transcripts() { return transcripts_; }

    uint64_t numAssignedFragments() { return numAssignedFragments_; }
    uint64_t numMappedReads() { return numAssignedFragments_; }


    std::atomic<uint64_t>& numAssignedFragmentsAtomic() { return numAssignedFragments_; }
    std::atomic<uint64_t>& batchNumAtomic() { return batchNum_; }

    template <typename CallbackT>
    bool processReads(const uint32_t& numThreads, CallbackT& processReadLibrary) {
        bool burnedIn = (totalAssignedFragments_ + numAssignedFragments_ > 5000000);
        for (auto& rl : readLibraries_) {
            processReadLibrary(rl, idx_, transcripts_, clusterForest(),
                               numAssignedFragments_, batchNum_, numThreads, burnedIn);
        }
        return true;
    }

    ~ReadExperiment() {
        // ---- Get rid of things we no longer need --------
        bwa_idx_destroy(idx_);
    }

    ClusterForest& clusterForest() { return *clusters_.get(); }

    std::string readFilesAsString() {
        std::stringstream sstr;
        size_t ln{0};
        size_t numReadLibraries{readLibraries_.size()};

        for (auto &rl : readLibraries_) {
            sstr << rl.readFilesAsString();
            if (ln++ < numReadLibraries) { sstr << "; "; }
        }
        return sstr.str();
    }

    bool softReset() {
        numObservedFragments_ = 0;
        totalAssignedFragments_ += numAssignedFragments_;
        numAssignedFragments_ = 0;
        // batchNum_ = 0; # don't reset batch num right now!
        quantificationPasses_++;
        return true;
    }

    bool reset() {
        namespace bfs = boost::filesystem;
        for (auto& rl : readLibraries_) {
            if (!rl.isRegularFile()) { return false; }
        }

        numObservedFragments_ = 0;
        totalAssignedFragments_ += numAssignedFragments_;
        numAssignedFragments_ = 0;
        // batchNum_ = 0; # don't reset batch num right now!
        quantificationPasses_++;
        return true;
    }

    void summarizeLibraryTypeCounts(boost::filesystem::path& opath){
        LibraryFormat fmt1(ReadType::SINGLE_END, ReadOrientation::NONE, ReadStrandedness::U);
        LibraryFormat fmt2(ReadType::SINGLE_END, ReadOrientation::NONE, ReadStrandedness::U);

        std::ofstream ofile(opath.string());

        fmt::MemoryWriter errstr;

        auto log = spdlog::get("jointLog");

        uint64_t numFmt1{0};
        uint64_t numFmt2{0};
        uint64_t numAgree{0};
        uint64_t numDisagree{0};

        for (auto& rl : readLibraries_) {
            auto fmt = rl.format();
            auto& counts = rl.libTypeCounts();

            // If the format is un-stranded, check that
            // we have a similar number of mappings in both
            // directions and then aggregate the forward and
            // reverse counts.
            if (fmt.strandedness == ReadStrandedness::U) {
                std::vector<ReadStrandedness> strands;
                switch (fmt.orientation) {
                    case ReadOrientation::SAME:
                    case ReadOrientation::NONE:
                        strands.push_back(ReadStrandedness::S);
                        strands.push_back(ReadStrandedness::A);
                        break;
                    case ReadOrientation::AWAY:
                    case ReadOrientation::TOWARD:
                        strands.push_back(ReadStrandedness::AS);
                        strands.push_back(ReadStrandedness::SA);
                        break;
                }

                fmt1.type = fmt.type; fmt1.orientation = fmt.orientation;
                fmt1.strandedness = strands[0];
                fmt2.type = fmt.type; fmt2.orientation = fmt.orientation;
                fmt2.strandedness = strands[1];

                numFmt1 = 0;
                numFmt2 = 0;
                numAgree = 0;
                numDisagree = 0;

                for (size_t i = 0; i < counts.size(); ++i) {
                    if (i == fmt1.formatID()) {
                        numFmt1 = counts[i];
                    } else if (i == fmt2.formatID()) {
                        numFmt2 = counts[i];
                    } else {
                        numDisagree += counts[i];
                    }
                }
                numAgree = numFmt1 + numFmt2;
                double ratio = static_cast<double>(numFmt1) / (numFmt1 + numFmt2);

                if ( std::abs(ratio - 0.5) > 0.01) {
                    errstr << "NOTE: Read Lib [" << rl.readFilesAsString() << "] :\n";
                    errstr << "\nDetected a strand bias > 1\% in an unstranded protocol "
                           << "check the file: " << opath.string() << " for details\n";

                    log->warn(errstr.str());
                    errstr.clear();
                }

                ofile << "========\n"
                      << "Read library consisting of files: "
                      << rl.readFilesAsString()
                      << "\n\n"
                      << "Expected format: " << rl.format()
                      << "\n\n"
                      << "# of consistent alignments: " << numAgree << "\n"
                      << "# of inconsistent alignments: " << numDisagree << "\n"
                      << "strand bias = " << ratio << " (0.5 is unbiased)\n"
                      << "# alignments with format " << fmt1 << ": " << numFmt1 << "\n"
                      << "# alignments with format " << fmt2 << ": " << numFmt2 << "\n"
                      << "\n========\n";
            } else {
                numAgree = 0;
                numDisagree = 0;

                for (size_t i = 0; i < counts.size(); ++i) {
                    if (i == fmt.formatID()) {
                        numAgree = counts[i];
                    } else {
                        numDisagree += counts[i];
                    }
                } // end for

                ofile << "========\n"
                      << "Read library consisting of files: "
                      << rl.readFilesAsString()
                      << "\n\n"
                      << "Expected format: " << rl.format()
                      << "\n\n"
                      << "# of consistent alignments: " << numAgree << "\n"
                      << "# of inconsistent alignments: " << numDisagree << "\n"
                      << "\n========\n";

            } //end else

            double disagreeRatio = static_cast<double>(numDisagree) / (numAgree + numDisagree);
            if (disagreeRatio > 0.05) {
                errstr << "NOTE: Read Lib [" << rl.readFilesAsString() << "] :\n";
                errstr << "\nGreater than 5\% of the alignments (but not, necessarily reads) "
                       << "disagreed with the provided library type; "
                       << "check the file: " << opath.string() << " for details\n";

                log->warn(errstr.str());
                errstr.clear();
            }

            ofile << "---- counts for each format type ---\n";
            for (size_t i = 0; i < counts.size(); ++i) {
                ofile << LibraryFormat::formatFromID(i) << " : " << counts[i] << "\n";
            }
            ofile << "------------------------------------\n\n";
        }
        ofile.close();
    }

    std::vector<ReadLibrary>& readLibraries() { return readLibraries_; }

    private:
    /**
     * The file from which the alignments will be read.
     * This can be a SAM or BAM file, and can be a regular
     * file or a fifo.
     */
    std::vector<ReadLibrary> readLibraries_;
    /**
     * The file from which the transcripts are read.
     * This is expected to be a FASTA format file.
     */
    //boost::filesystem::path transcriptFile_;
    /**
     * The targets (transcripts) to be quantified.
     */
    std::vector<Transcript> transcripts_;
    /**
     * The index we've built on the set of transcripts.
     */
    bwaidx_t *idx_{nullptr};
    /**
     * The cluster forest maintains the dynamic relationship
     * defined by transcripts and reads --- if two transcripts
     * share an ambiguously mapped read, then they are placed
     * in the same cluster.
     */
    std::unique_ptr<ClusterForest> clusters_;
    /** Keeps track of the number of passes that have been
     *  made through the alignment file.
     */
    std::atomic<uint64_t> numObservedFragments_{0};
    std::atomic<uint64_t> numAssignedFragments_{0};
    std::atomic<uint64_t> batchNum_{0};
    uint64_t totalAssignedFragments_;
    size_t quantificationPasses_;
};

#endif // EXPERIMENT_HPP
