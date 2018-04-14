//------------------------------------------------------------------------------
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.
//
// © H2O.ai 2018
//------------------------------------------------------------------------------
#include "csv/reader.h"
#include <algorithm>           // std::max
#include "csv/reader_fread.h"  // FreadReader, FreadLocalParseContext
#include "utils/assert.h"



//------------------------------------------------------------------------------
// ChunkedDataReader
//------------------------------------------------------------------------------

ChunkedDataReader::ChunkedDataReader(GenericReader& reader, double meanLineLen)
  : g(reader)
{
  chunkSize = 0;
  chunkCount = 0;
  inputStart = g.sof;
  inputEnd = g.eof;
  lastChunkEnd = inputStart;
  lineLength = std::max(meanLineLen, 1.0);
  nthreads = g.nthreads;
  nrows_written = 0;
  nrows_allocated = g.columns.get_nrows();
  nrows_max = g.max_nrows;
  xassert(nrows_allocated <= nrows_max);

  determine_chunking_strategy();
}


void ChunkedDataReader::determine_chunking_strategy() {
  size_t inputSize = static_cast<size_t>(inputEnd - inputStart);
  size_t size1000 = static_cast<size_t>(1000 * lineLength);
  size_t zThreads = static_cast<size_t>(nthreads);
  chunkSize = std::max<size_t>(size1000, 1 << 18);
  chunkCount = std::max<size_t>(inputSize / chunkSize, 1);
  if (chunkCount > zThreads) {
    chunkCount = zThreads * (1 + (chunkCount - 1)/zThreads);
  } else {
    nthreads = static_cast<int>(chunkCount);
    g.trace("Number of threads reduced to %d because data is small",
            nthreads);
  }
  chunkSize = inputSize / chunkCount;
}



ChunkCoordinates ChunkedDataReader::compute_chunk_boundaries(
  size_t i, LocalParseContext* ctx) const
{
  xassert(i < chunkCount);
  ChunkCoordinates c;

  bool isFirstChunk = (i == 0);
  bool isLastChunk = (i == chunkCount - 1);

  if (nthreads == 1 || isFirstChunk) {
    c.start = lastChunkEnd;
    c.true_start = true;
  } else {
    c.start = inputStart + i * chunkSize;
  }
  if (isLastChunk) {
    c.end = inputEnd;
    c.true_end = true;
  } else {
    c.end = c.start + chunkSize;
  }

  adjust_chunk_coordinates(c, ctx);

  return c;
}


double ChunkedDataReader::work_done_amount() const {
  double done = static_cast<double>(lastChunkEnd - inputStart);
  double total = static_cast<double>(inputEnd - inputStart);
  return done / total;
}


void ChunkedDataReader::adjust_chunk_coordinates(
      ChunkCoordinates&, LocalParseContext*) const {}




void ChunkedDataReader::read_all()
{
  // Any exceptions that are thrown within OMP blocks must be captured within
  // the same block. If allowed to propagate, they may corrupt the stack and
  // crash the program. This is why we have all code surrounded with
  // try-catch clauses, and `OmpExceptionManager` to help us remember the
  // exception that was thrown and manually propagate it outwards.
  OmpExceptionManager oem;

  #pragma omp parallel num_threads(nthreads)
  {
    bool tMaster = false;
    #pragma omp master
    {
      int actual_nthreads = omp_get_num_threads();
      if (actual_nthreads != nthreads) {
        nthreads = actual_nthreads;
        g.trace("Actual number of threads allowed by OMP: %d", nthreads);
        determine_chunking_strategy();
      }
      tMaster = true;
    }
    // Wait for master here: we want all threads to have consistent
    // view of the chunking parameters.
    #pragma omp barrier

    // These variables control how the progress bar is shown. `tShowProgress`
    // is the main flag telling us whether the progress bar should be shown
    // or not by the current thread (note that only master thread can have
    // this flag on -- this is because progress-reporting reaches to python
    // runtime, and we can do that from a single thread only). When
    // `tShowProgress` is on, the flag `tShowAlways` controls whether we need
    // to show the progress right away, or wait until time moment `tShowWhen`.
    // This is needed because we don't want the progress bar for really small
    // and fast files. However if the file is big enough (>256MB) then it's ok
    // to show the progress as soon as possible.
    bool tShowProgress = g.report_progress && tMaster;
    bool tShowAlways = tShowProgress && (inputEnd - inputStart > (1 << 28));
    double tShowWhen = tShowProgress? wallclock() + 0.75 : 0;

    // Thread-local parse context. This object does most of the parsing job.
    auto tctx = init_thread_context();

    // Helper variables for keeping track of chunk's coordinates:
    // `txcc` has the expected chunk coordinates (i.e. as determined ex ante
    // in `compute_chunk_coordinates()`), and `tacc` the actual chunk
    // coordinates (i.e. how much data was actually read in `read_chunk()`).
    // These two are very often the same; however when they do differ, it is
    // the job of `order_chunk()` to reconcile the differences.
    ChunkCoordinates txcc;
    ChunkCoordinates tacc;

    // Main data reading loop
    #pragma omp for ordered schedule(dynamic)
    for (size_t i = 0; i < chunkCount; ++i) {
      if (oem.exception_caught()) continue;
      try {
        if (tShowAlways || (tShowProgress && wallclock() >= tShowWhen)) {
          g.progress(work_done_amount());
          tShowAlways = true;
        }

        tctx->push_buffers();
        txcc = compute_chunk_boundaries(i, tctx.get());
        tctx->read_chunk(txcc, tacc);

      } catch (...) {
        oem.capture_exception();
      }

      #pragma omp ordered
      do {
        if (oem.exception_caught()) break;
        try {
          tctx->row0 = nrows_written;
          order_chunk(tacc, txcc, tctx);

          size_t nrows_new = nrows_written + tctx->used_nrows;
          if (nrows_new > nrows_allocated) {
            if (nrows_allocated == nrows_max) {
              // nrows_allocated is the same as nrows_max, no need to reallocate
              // the output, just truncate the rows in the current chunk.
              tctx->used_nrows = nrows_allocated - nrows_written;
              nrows_new = nrows_allocated;
            } else {
              realloc_output_columns(i, nrows_new);
            }
          }
          nrows_written = nrows_new;

          tctx->orderBuffer();

        } catch (...) {
          oem.capture_exception();
        }
      } while (0);  // #pragma omp ordered
    }  // #pragma omp for ordered

    // Stopped early because of error. Discard the content of the buffers,
    // because they were not ordered, and trying to push them may lead to
    // unexpected bugs...
    if (oem.exception_caught()) {
      tctx->used_nrows = 0;
    }

    // Push out the buffers one last time.
    if (tctx->used_nrows) {
      try {
        tctx->push_buffers();
      } catch (...) {
        oem.capture_exception();
      }
    }

    // Report progress one last time
    if (tShowAlways) {
      int status = 1 + oem.exception_caught() + oem.is_keyboard_interrupt();
      g.progress(work_done_amount(), status);
    }
  }  // #pragma omp parallel

  // If any exception occurred, propagate it to the caller
  oem.rethrow_exception_if_any();

  // Reallocate the output to have the correct number of rows
  g.columns.set_nrows(nrows_written);

  // Check that all input was read (unless interrupted early because of
  // nrows_max).
  if (nrows_written < nrows_max) {
    xassert(lastChunkEnd == inputEnd);
  }
}



void ChunkedDataReader::realloc_output_columns(size_t ichunk, size_t new_alloc)
{
  if (ichunk == chunkCount - 1) {
    // If we're on the last jump, then `new_alloc` is exactly how many rows
    // will be needed.
  } else {
    // Otherwise we adjust the alloc to account for future chunks as well.
    double exp_nrows = 1.2 * new_alloc * chunkCount / (ichunk + 1);
    new_alloc = std::max(static_cast<size_t>(exp_nrows),
                         1024 + nrows_allocated);
  }
  if (new_alloc > nrows_max) {
    new_alloc = nrows_max;
  }
  nrows_allocated = new_alloc;
  g.trace("Too few rows allocated, reallocating to %zu rows", nrows_allocated);

  dt::shared_lock(shmutex, /* exclusive = */ true);
  g.columns.set_nrows(nrows_allocated);
}


void ChunkedDataReader::order_chunk(
  ChunkCoordinates& acc, ChunkCoordinates& xcc, LocalParseContextPtr& ctx)
{
  int i = 2;
  while (i--) {
    if (acc.start == lastChunkEnd && acc.end >= lastChunkEnd) {
      lastChunkEnd = acc.end;
      return;
    }
    xcc.start = lastChunkEnd;
    xcc.true_start = true;

    ctx->read_chunk(xcc, acc);
    xassert(i);
  }
}
