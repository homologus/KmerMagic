#ifndef ALIGNMENT_GROUP
#define ALIGNMENT_GROUP

extern "C" {
#include "io_lib/scram.h"
#include "io_lib/os.h"
}

// Cereal includes
#include "cereal/types/vector.hpp"
#include "cereal/archives/binary.hpp"

#include <vector>
#include "SailfishMath.hpp"
#include "ReadPair.hpp"

template <typename FragT>
class AlignmentGroup {
    public:
        AlignmentGroup() : read_(nullptr) { alignments_.reserve(10); }

        void setRead(std::string* r) { read_ = r; }
        std::string* read() { return read_; }

        inline std::vector<FragT>& alignments() { return alignments_; }
        void addAlignment(FragT p) { alignments_.push_back(p); }
        inline size_t numAlignments() { return alignments_.size(); }
        inline size_t size() { return numAlignments(); }

        template <typename Archive>
        void serialize(Archive& archive) {
            archive(alignments_);
        }

    private:
        std::vector<FragT> alignments_;
        std::string* read_;
};
#endif // ALIGNMENT_GROUP
