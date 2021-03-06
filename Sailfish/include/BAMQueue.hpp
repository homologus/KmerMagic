#ifndef __BAMQUEUE_HPP__
#define __BAMQUEUE_HPP__

#include <boost/lockfree/spsc_queue.hpp>
#include <boost/lockfree/queue.hpp>
#include <tbb/atomic.h>
#include <iostream>
#include <fstream>
#include <atomic>
#include <vector>
#include <memory>
#include <exception>
#include <thread>
#include <boost/timer/timer.hpp>
#include <boost/filesystem.hpp>
#include <tbb/concurrent_queue.h>
#include "AlignmentGroup.hpp"
#include "LibraryFormat.hpp"
#include "SailfishMath.hpp"
#include "ReadPair.hpp"
#include "UnpairedRead.hpp"
#include "spdlog/spdlog.h"

extern "C" {
#include "io_lib/scram.h"
#include "io_lib/os.h"
#undef max
#undef min
}

/**
  * Simple structure holding info about the alignment file.
  */
struct AlignmentFile {
    boost::filesystem::path fileName;
    std::string readMode;
    scram_fd* fp;
    SAM_hdr* header;
    uint32_t numParseThreads;
};

/**
  * A queue from which to draw BAM alignments.  The queue is thread-safe, and
  * can be written to and read from multiple threads.
  *
  * This class is templated on LibT --- the type of read library from which
  * the provided alignments are being generated.
  */
template <typename FragT>
class BAMQueue {
public:
  BAMQueue(std::vector<boost::filesystem::path>& fnames, LibraryFormat& libFmt, uint32_t numParseThreads);
  ~BAMQueue();
  void forceEndParsing();

  SAM_hdr* header();
  SAM_hdr* safeHeader();

  std::vector<SAM_hdr*> headers();

  template <typename FilterT>
  void start(FilterT filt);

  inline bool getAlignmentGroup(AlignmentGroup<FragT*>*& group);

  // Return the number of reads processed so far by the queue
  size_t numObservedReads();
  size_t numMappedReads();

  void reset();

  tbb::concurrent_bounded_queue<FragT*>& getFragmentQueue();
  tbb::concurrent_bounded_queue<AlignmentGroup<FragT*>*>& getAlignmentGroupQueue();

private:
  size_t popNum{0};
  /** Fill the queue with the appropriate type of alignment
   * depending on the template paramater T
   */
  template <typename FilterT>
  void fillQueue_(FilterT);

  /** Overload of getFrag_ for paired-end reads */
  template <typename FilterT>
  inline bool getFrag_(ReadPair& rpair, FilterT filt);
  /** Overload of getFrag_ for single-end reads */
  template <typename FilterT>
  inline bool getFrag_(UnpairedRead& sread, FilterT filt);

public:
  bool verbose=false;
private:
  std::vector<AlignmentFile> files_;
  std::string fname_;
  LibraryFormat libFmt_;

  std::vector<AlignmentFile>::iterator currFile_;
  scram_fd* fp_ = nullptr;
  SAM_hdr* hdr_ = nullptr;

  //htsFile* fp_ = nullptr;
  size_t totalReads_;
  size_t numUnaligned_;
  size_t numMappedReads_;
  tbb::concurrent_bounded_queue<FragT*> fragmentQueue_;
  tbb::concurrent_bounded_queue<AlignmentGroup<FragT*>*> alnGroupPool_;
  //tbb::concurrent_bounded_queue<AlignmentGroup<FragT>*> alnGroupQueue_;
  boost::lockfree::spsc_queue<AlignmentGroup<FragT*>*,
                              boost::lockfree::capacity<65535>> alnGroupQueue_;
  volatile bool doneParsing_;
  std::unique_ptr<std::thread> parsingThread_;
  std::shared_ptr<spdlog::logger> logger_;

  size_t batchNum_;
  std::string readMode_;
};

#include "BAMQueue.tpp"
#endif //__BAMQUEUE_HPP__
