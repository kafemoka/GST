#include "gtest/gtest.h"

#include <numeric>
#include <vector>

#include "ans_encode.h"
#include "ans_ocl.h"
#include "bits.h"
#include "histogram.h"
#include "kernel_cache.h"

using ans::OpenCLDecoder;

static class OpenCLEnvironment : public ::testing::Environment {
public:
  OpenCLEnvironment() : ::testing::Environment() { is_setup = false;  }
  virtual ~OpenCLEnvironment() { }
  virtual void SetUp() {
    _ctx = std::move(gpu::GPUContext::InitializeOpenCL(false));
    // Make sure to always initialize random number generator for
    // deterministic tests
    srand(0);
    is_setup = true;
  }

  virtual void TearDown() {
    _ctx = nullptr;
    is_setup = false;
  }

  const std::unique_ptr<gpu::GPUContext> &GetContext() const { assert(is_setup);  return _ctx; }

private:
  bool is_setup;
  std::unique_ptr<gpu::GPUContext> _ctx;
} *gTestEnv;

static std::vector<cl_uchar> GenerateSymbols(const std::vector<int> &freqs, size_t num_symbols) {
  assert(freqs.size() < 256);
  std::vector<cl_uchar> symbols;
  symbols.reserve(num_symbols);

  for (int i = 0; i < num_symbols; ++i) {
    uint32_t M = std::accumulate(freqs.begin(), freqs.end(), 0);
    int r = rand() % M;
    int symbol = 0;
    int freq = 0;
    for (auto f : freqs) {
      freq += f;
      if (r < freq) {
        break;
      }
      symbol++;
    }
    assert(symbol < freqs.size());
    symbols.push_back(symbol);
  }

  return std::move(symbols);
}

TEST(ANS_OpenCL, Initialization) {
  std::vector<int> F = { 3, 2, 1, 4, 3 };
  OpenCLDecoder decoder(gTestEnv->GetContext(), F, 1);

  std::vector<int> normalized_F = ans::GenerateHistogram(F, ans::kANSTableSize);

  std::vector<cl_uchar> expected_symbols(ans::kANSTableSize, 0);
  std::vector<cl_ushort> expected_frequencies(ans::kANSTableSize, 0);
  std::vector<cl_ushort> expected_cumulative_frequencies(ans::kANSTableSize, 0);

  int sum = 0;
  for (size_t i = 0; i < normalized_F.size(); ++i) {
    for (size_t j = 0; j < normalized_F[i]; ++j) {
      expected_symbols[sum + j] = static_cast<cl_uchar>(i);
      expected_frequencies[sum + j] = normalized_F[i];
      expected_cumulative_frequencies[sum + j] = sum;
    }
    sum += normalized_F[i];
  }
  ASSERT_EQ(sum, ans::kANSTableSize);

  std::vector<cl_uchar> symbols = std::move(decoder.GetSymbols());
  std::vector<cl_ushort> frequencies = std::move(decoder.GetFrequencies());
  std::vector<cl_ushort> cumulative_frequencies = std::move(decoder.GetCumulativeFrequencies());

  ASSERT_EQ(symbols.size(), expected_symbols.size());
  for (size_t i = 0; i < symbols.size(); ++i) {
    EXPECT_EQ(expected_symbols[i], symbols[i]) << "Symbols differ at index " << i;
  }

  ASSERT_EQ(frequencies.size(), expected_frequencies.size());
  for (size_t i = 0; i < frequencies.size(); ++i) {
    EXPECT_EQ(expected_frequencies[i], frequencies[i]) << "Frequencies differ at index " << i;
  }

  ASSERT_EQ(cumulative_frequencies.size(), expected_cumulative_frequencies.size());
  for (size_t i = 0; i < cumulative_frequencies.size(); ++i) {
    EXPECT_EQ(expected_cumulative_frequencies[i], cumulative_frequencies[i])
      << "Cumulative frequencies differ at index " << i;
  }
}

TEST(ANS_OpenCL, TableRebuilding) {
  std::vector<int> F = { 3, 2, 1, 4, 3, 406 };
  std::vector<int> new_F = { 80, 300, 2, 14, 1, 1, 1, 20 };
  std::vector<int> normalized_F = ans::GenerateHistogram(new_F, ans::kANSTableSize);

  OpenCLDecoder decoder(gTestEnv->GetContext(), F, 1);
  decoder.RebuildTable(new_F);

  std::vector<cl_uchar> expected_symbols(ans::kANSTableSize, 0);
  std::vector<cl_ushort> expected_frequencies(ans::kANSTableSize, 0);
  std::vector<cl_ushort> expected_cumulative_frequencies(ans::kANSTableSize, 0);

  int sum = 0;
  for (size_t i = 0; i < normalized_F.size(); ++i) {
    for (size_t j = 0; j < normalized_F[i]; ++j) {
      expected_symbols[sum + j] = static_cast<cl_uchar>(i);
      expected_frequencies[sum + j] = normalized_F[i];
      expected_cumulative_frequencies[sum + j] = sum;
    }
    sum += normalized_F[i];
  }
  ASSERT_EQ(sum, ans::kANSTableSize);

  ASSERT_EQ(expected_symbols.size(), ans::kANSTableSize);
  ASSERT_EQ(expected_frequencies.size(), ans::kANSTableSize);
  ASSERT_EQ(expected_cumulative_frequencies.size(), ans::kANSTableSize);

  std::vector<cl_uchar> symbols = std::move(decoder.GetSymbols());
  std::vector<cl_ushort> frequencies = std::move(decoder.GetFrequencies());
  std::vector<cl_ushort> cumulative_frequencies = std::move(decoder.GetCumulativeFrequencies());

  ASSERT_EQ(symbols.size(), expected_symbols.size());
  for (size_t i = 0; i < symbols.size(); ++i) {
    EXPECT_EQ(expected_symbols[i], symbols[i]) << "Symbols differ at index " << i;
  }

  ASSERT_EQ(frequencies.size(), expected_frequencies.size());
  for (size_t i = 0; i < frequencies.size(); ++i) {
    EXPECT_EQ(expected_frequencies[i], frequencies[i]) << "Frequencies differ at index " << i;
  }

  ASSERT_EQ(cumulative_frequencies.size(), expected_cumulative_frequencies.size());
  for (size_t i = 0; i < cumulative_frequencies.size(); ++i) {
    EXPECT_EQ(expected_cumulative_frequencies[i], cumulative_frequencies[i])
      << "Cumulative frequencies differ at index " << i;
  }
}

TEST(ANS_OpenCL, DecodeSingleStream) {
  std::vector<int> F = { 12, 14, 17, 1, 1, 2, 372 };

  const size_t num_symbols = 256;
  std::vector<uint8_t> symbols = std::move(GenerateSymbols(F, ans::kNumEncodedSymbols));
  ASSERT_EQ(symbols.size(), ans::kNumEncodedSymbols);

  ans::OpenCLEncoder encoder(F);
  std::vector<cl_uchar> stream(10);

  size_t bytes_written = 0;
  for (auto symbol : symbols) {
    ans::BitWriter w(stream.data() + bytes_written);
    encoder.Encode(symbol, w);

    bytes_written += w.BytesWritten();
    if (bytes_written > (stream.size() / 2)) {
      stream.resize(stream.size() * 2);
    }
  }

  stream.resize(bytes_written);
  ASSERT_EQ(bytes_written % 2, 0);

  // First, make sure we can CPU decode it.
  std::vector<uint16_t> short_stream;
  short_stream.reserve(bytes_written / 2);
  for (int i = 0; i < bytes_written; i += 2) {
    uint16_t x = (static_cast<uint16_t>(stream[i + 1]) << 8) | stream[i];
    short_stream.push_back(x);
  }
  std::reverse(short_stream.begin(), short_stream.end());

  std::vector<uint8_t> cpu_decoded_symbols;
  cpu_decoded_symbols.reserve(ans::kNumEncodedSymbols);

  ans::BitReader r(reinterpret_cast<const uint8_t *>(short_stream.data()));
  ans::OpenCLCPUDecoder cpu_decoder(encoder.GetState(), F);
  for (int i = 0; i < ans::kNumEncodedSymbols; ++i) {
    cpu_decoded_symbols.push_back(static_cast<uint8_t>(cpu_decoder.Decode(r)));
  }
  std::reverse(cpu_decoded_symbols.begin(), cpu_decoded_symbols.end()); // Decode in reverse
  ASSERT_EQ(cpu_decoded_symbols.size(), ans::kNumEncodedSymbols);
  for (size_t i = 0; i < ans::kNumEncodedSymbols; ++i) {
    EXPECT_EQ(cpu_decoded_symbols[i], symbols[i]) << "Symbols differ at index " << i;
  }

  // Now make sure we can GPU decode it
  ans::OpenCLDecoder decoder(gTestEnv->GetContext(), F, 1);
  std::vector<cl_uchar> decoded_symbols = std::move(decoder.Decode(encoder.GetState(), stream));
  ASSERT_EQ(decoded_symbols.size(), ans::kNumEncodedSymbols);
  for (size_t i = 0; i < ans::kNumEncodedSymbols; ++i) {
    EXPECT_EQ(decoded_symbols[i], symbols[i]) << "Symbols differ at index " << i;
  }

  // Make sure we can decode it twice...
  ans::OpenCLDecoder decoder2(gTestEnv->GetContext(), F, 1);
  decoded_symbols = std::move(decoder2.Decode(encoder.GetState(), stream));
  ASSERT_EQ(decoded_symbols.size(), ans::kNumEncodedSymbols);
  for (size_t i = 0; i < ans::kNumEncodedSymbols; ++i) {
    EXPECT_EQ(decoded_symbols[i], symbols[i]) << "Symbols differ at index " << i;
  }
}

TEST(ANS_OpenCL, DecodeInterleavedStreams) {
  std::vector<int> F = { 32, 186, 54, 8, 94, 35, 13, 21, 456, 789, 33, 215, 6, 54, 987, 54, 65, 13, 2, 1 };
  const int num_interleaved = 24;

  std::vector<cl_uchar> symbols[num_interleaved];
  for (int i = 0; i < num_interleaved; ++i) {
    symbols[i] = std::move(GenerateSymbols(F, ans::kNumEncodedSymbols));
    ASSERT_EQ(symbols[i].size(), ans::kNumEncodedSymbols);
  }

  std::vector<ans::OpenCLEncoder> encoders;
  encoders.reserve(num_interleaved);
  for (int i = 0; i < num_interleaved; ++i) {
    encoders.push_back(std::move(ans::OpenCLEncoder(F)));
  }

  std::vector<cl_uchar> stream(10);

  size_t bytes_written = 0;
  for (int sym_idx = 0; sym_idx < ans::kNumEncodedSymbols; ++sym_idx) {
    for (int strm_idx = 0; strm_idx < num_interleaved; ++strm_idx) {
      ans::BitWriter w(stream.data() + bytes_written);
      encoders[strm_idx].Encode(symbols[strm_idx][sym_idx], w);

      bytes_written += w.BytesWritten();
      if (bytes_written >(stream.size() / 2)) {
        stream.resize(stream.size() * 2);
      }
    }
  }
  stream.resize(bytes_written);
  ASSERT_EQ(bytes_written % 2, 0);

  // Let's get our states...
  std::vector<cl_uint> states(num_interleaved, 0);
  std::transform(encoders.begin(), encoders.end(), states.begin(),
    [](const ans::OpenCLEncoder &enc) { return enc.GetState(); }
  );

  // Now decode it!
  ans::OpenCLDecoder decoder(gTestEnv->GetContext(), F, 1);
  std::vector<std::vector<cl_uchar> > decoded_symbols = std::move(decoder.Decode(states, stream));
  ASSERT_EQ(decoded_symbols.size(), num_interleaved);
  for (int strm_idx = 0; strm_idx < num_interleaved; ++strm_idx) {
    ASSERT_EQ(decoded_symbols[strm_idx].size(), ans::kNumEncodedSymbols)
      << "Issue with decoded stream at index: " << strm_idx;
  }

  for (int sym_idx = 0; sym_idx < ans::kNumEncodedSymbols; ++sym_idx) {
    for (int strm_idx = 0; strm_idx < num_interleaved; ++strm_idx) {
      EXPECT_EQ(decoded_symbols[strm_idx][sym_idx], symbols[strm_idx][sym_idx])
        << "Symbols differ at stream and index: " << (strm_idx, sym_idx);
    }
  }
}

TEST(ANS_OpenCL, DecodeMultipleInterleavedStreams) {
  // !FIXME! Write me...
  FAIL();
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  gTestEnv = dynamic_cast<OpenCLEnvironment *>(
    ::testing::AddGlobalTestEnvironment(new OpenCLEnvironment));
  assert(NULL != gTestEnv);
  return RUN_ALL_TESTS();
}
