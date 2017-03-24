/*
We could have tracked `query index` in DocWordsSpace instead of `term IDs`, which would have made it trivial to perform proximity checks in MatchedIndexDocumentsFilter::consider(), but the downsides make it a bad ideal;
- slower Trinity::Codecs::Decoder::materialize_hits() 
- require a more elaborate materialize_hits() signature
- slower phrasematch_impl()
Given the above, and that the fact that not all consider() implementations will perform proximity checks for score computations, we settled for an alternative which doesn't require any changes, and accept that proximity checks are only trivially harder to perform. 
We will simply identify all distinct termIDs (usually just one) for each query index and will look up the termIDs in that list.
*/
#pragma once
#include "common.h"
#include "runtime.h"

namespace Trinity
{
	class DocWordsSpace final
        {
              private:
	      	// just 4 bytes/position
		// we could have separated docSeq and termID into different arrays(as in, not in the same struct) so that reset()
		// would only memset() the docSeq array(2 bytes vs 4 bytes) * maxPos
		// but that 'd make access somewhat slower for set() and test() because we 'd get more cache misses so we optimise for it
                struct position
                {
                        uint16_t docSeq;
                        exec_term_id_t termID;  // See IMPL.md
                };

                position *const positions;
                const uint32_t maxPos;
                uint16_t curSeq;


              public:
		// allocating max + Trinity::Limits::MaxPhraseSize, because that is the theoritical maximum phrase size
		// and if we are going to test starting from maxPos extending to 10 positions ahead, we want to
		// make sure we won't read outside positions. 
		// The extra positions will be always initialized to 0 and we won't need to reset those in reset()
                DocWordsSpace(const uint32_t max = Trinity::Limits::MaxPosition)
			: positions((position *)calloc(sizeof(position), max + 1 + Trinity::Limits::MaxPhraseSize)), maxPos{max} 
		{
			curSeq = 1; // IMPORTANT, start from (1)
			require(max && max <= Trinity::Limits::MaxPosition);
		}

		~DocWordsSpace()
		{
                        free(positions);
		}

		void reset()
		{
			// In order to avoid resetting/clearing positions[] for every other document
			// we track a document-specific identifier in positions[] so if positions[idx].docSeq != curDocSeq
			// then whatever is in positions[] is stale and should be considered unset.
			//
			// In a previous Trinity design we stored the document ID as u32 but that is excessive, we care about cache-misses
			// so now we instead use a `uint16_t curSeq` and periodically clear. This is more efficient
                        if (unlikely(curSeq == UINT16_MAX))
                        {
				// we reset every 65k documents
				// this is preferrable to using uint32_t to encode the actual document in position{}
				// no need to memset() for (maxPos + 1 + Trinity::Limits::MaxPhraseSize), just upto (maxPos + 1)
                                memset(positions, 0, sizeof(position) * (maxPos + 1));
                                curSeq = 1; 	// important; set to 1 not 0
                        }
                        else
                                ++curSeq;
		}

		// XXX: pos must be > 0
		[[gnu::always_inline]] void set(const exec_term_id_t termID, const tokenpos_t pos) noexcept
		{
                        positions[pos] = {curSeq, termID};
		}

#if 1
		/*
		 * -O1

        movq    (%rdi), %rax
        movzwl  (%rax,%rdx,4), %ecx
        cmpw    12(%rdi), %cx
        jne     .LBB3_1
        cmpw    %si, 2(%rax,%rdx,4)
        sete    %al
        retq
.LBB3_1:
        xorl    %eax, %eax
        retq
		*
		*/

		// Turns out with -O1 or higher, this is faster than the alternative impl
		// based on my folly benchmarks
		// XXX: pos must be > 0
		inline bool test(const exec_term_id_t termID, const tokenpos_t pos) const noexcept
		{
			return positions[pos].docSeq == curSeq && positions[pos].termID == termID;
		}


#else
		/* 
		 * -O1

        shll    $16, %esi
        movzwl  12(%rdi), %eax
        orl     %esi, %eax
        movq    (%rdi), %rcx
        cmpl    (%rcx,%rdx,4), %eax
        sete    %al
        retq

		*
 		*/	
		inline bool test(const exec_term_id_t termID, const uint16_t pos) const noexcept
		{
			// this works thanks on little-endian arhs.
			// not sure if this is faster than the older impl.
			// in -O1, this saves us 2 instructions and looks faster. Will investigate
			// 
			// TODO: We could in theory use this trick to check for _2_ positions ahead by
			// building a u64 and using *(uint64_t *)&positions[pos] which will also
			// access the adjacent positions[pos+1].
			// e.g test2() method
			static_assert(sizeof(termID) == sizeof(uint16_t));
			static_assert(sizeof(curSeq) == sizeof(uint16_t));

			return ((uint32_t(termID) << 16) | curSeq) == *(uint32_t *)&positions[pos];
		}
#endif


		// This can facilitate tracking sequences(e.g 2+ qeury terms matches in a document) of a MatchedIndexDocumentsFilter::consider()  impl.
		inline void unset(const tokenpos_t pos) noexcept
                {
                        positions[pos].docSeq = 0;
                }

		// We can probably just sort all phrase terms by freq asc
		// and iterate across all hits, and for each hit, see if it is set
		// in the adjacement position for the next phrase term, and the next, and so on, but 
		// we would need to track the offset(relative index in the phrase)
		// This is an example/reference implementation
		bool test_phrase(const std::vector<exec_term_id_t> &phraseTerms, const tokenpos_t *phraseFirstTokenPositions, const tokenpos_t phraseFirstTokenPositionsCnt) const;
        };
}
