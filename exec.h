#pragma once
#include "queries.h"
#include "docidupdates.h"
#include "index_source.h"
#include "matches.h"
#include <future>

namespace Trinity
{
        void exec_query(const query &in, IndexSource *, masked_documents_registry *const maskedDocumentsRegistry, MatchedIndexDocumentsFilter *, IndexDocumentsFilter *const f = nullptr);

        // Handy utility function; executes query on all index sources in the provided collection in sequence and returns
        // a vector with the match filters/results of each execution.
        //
        // You are expected to merge/reduce/blend them.
        // It's trivial to do this in parallel using e.g std::async() or any other means of scheduling exec_query() for each index source in
        // a different thread. See exec_query_par() for a possible implementation.
	//
        // Note that execution of sources does not depend on state of other sources - they are isolated so parallel processing them requires
        // no coordination.
        template <typename T, typename... Arg>
        std::vector<std::unique_ptr<T>> exec_query(const query &in, IndexSourcesCollection *collection, IndexDocumentsFilter *f, Arg &&... args)
        {
                static_assert(std::is_base_of<MatchedIndexDocumentsFilter, T>::value, "Expected a MatchedIndexDocumentsFilter subclass");
                const auto n = collection->sources.size();
                std::vector<std::unique_ptr<T>> out;

                for (uint32_t i{0}; i != n; ++i)
                {
                        auto source = collection->sources[i];
                        auto scanner = collection->scanner_registry_for(i);
                        auto filter = std::make_unique<T>(std::forward<Arg>(args)...);

                        exec_query(in, source, scanner.get(), filter.get(), f);
                        out.push_back(std::move(filter));
                }

                return out;
        }

        // Parallel queries execution, using std::async()
        template <typename T, typename... Arg>
        std::vector<std::unique_ptr<T>> exec_query_par(const query &in, IndexSourcesCollection *collection, IndexDocumentsFilter *f, Arg &&... args)
        {
                static_assert(std::is_base_of<MatchedIndexDocumentsFilter, T>::value, "Expected a MatchedIndexDocumentsFilter subclass");
                const auto n = collection->sources.size();
                std::vector<std::unique_ptr<T>> out;

		if (!n)
			return out;
		else if (n == 1)
		{
			auto source = collection->sources[0];
			auto scanner = collection->scanner_registry_for(0);
			auto filter = std::make_unique<T>(std::forward<Arg>(args)...);

			exec_query(in, source, scanner.get(), filter.get(), f);
			out.push_back(std::move(filter));
			return out;
		}


                std::vector<std::future<std::unique_ptr<T>>> futures;

                for (uint32_t i{0}; i != n; ++i)
                {
                        futures.push_back(
                            std::async(std::launch::async, [&](const uint32_t i) {
                                    auto source = collection->sources[i];
                                    auto scanner = collection->scanner_registry_for(i);
                                    auto filter = std::make_unique<T>(std::forward<Arg>(args)...);

                                    exec_query(in, source, scanner.get(), filter.get(), f);
                                    return filter;
                            },
                                       i));
                }

                while (futures.size())
                {
                        auto &f = futures.back();

                        out.push_back(std::move(f.get()));
                        futures.pop_back();
                }

                return out;
        }
};
