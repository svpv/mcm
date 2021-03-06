/*	MCM file compressor

  Copyright (C) 2015, Google Inc.
  Authors: Mathieu Chartier

  LICENSE

    This file is part of the MCM file compressor.

    MCM is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    MCM is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with MCM.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef _WAV16_HPP_
#define _WAV16_HPP_

#include <cstdlib>
#include <vector>

#include "DivTable.hpp"
#include "Entropy.hpp"
#include "Huffman.hpp"
#include "Log.hpp"
#include "MatchModel.hpp"
#include "Memory.hpp"
#include "Mixer.hpp"
#include "Model.hpp"
#include "Range.hpp"
#include "StateMap.hpp"
#include "Util.hpp"
#include "WordModel.hpp"

class Wav16 : public Compressor {
public:
  // SS table
  static const uint32_t kShift = 12;
  static const uint32_t kMaxValue = 1 << kShift;
  typedef fastBitModel<int, kShift, 9, 30> StationaryModel;
  // typedef bitLearnModel<kShift, 8, 30> StationaryModel;

  static const size_t kSampleShift = 4;
  static const size_t kSamplePr = 16 - kSampleShift;
  static const size_t kSampleCount = 1u << kSamplePr;

  static const size_t kContextBits = 2;
  static const size_t kContextMask = (1u << kContextBits) - 1;
  std::vector<StationaryModel> models_;

  // Range encoder
  Range7 ent;

  // Optimization variable.
  uint32_t opt_var;

  // Noise bits which are just direct encoded.
  size_t noise_bits_;
  size_t non_noise_bits_;

  // OGD:
  // Cost x = lg(x)
  // Estimate e = w0 * s0 + w1 * s1 
  // Cost w0, w1 = lg|e - p| + 1
  // Diff w_i += a * (

  class LinearMixer {
    static const size_t kShift = 16;
    static const size_t kWeights = 4;
    int32_t w[kWeights];
    static const int64_t kMaxW = 1000000;
    static const int64_t kMinW = -1000000;
  public:
    LinearMixer() {
      for (auto& n : w) n = (1u << kShift) / kWeights;
    }

    void printWeights() {
      for (size_t i = 0; i < kWeights; ++i) {
        std::cout << w[i] << " ";
      }
    }

    int32_t mix(int32_t* s) {
      int64_t sum = 0;
      for (size_t i = 0; i < kWeights; ++i) {
        sum += static_cast<int64_t>(s[i]) * static_cast<int64_t>(w[i]);
      }
      sum >>= kShift;
      // if (sum < 0) sum = 0;
      // if (sum > 65535) sum = 65535;
      return sum;
    }

    void update(int32_t* s, int32_t p, int32_t error, size_t learn_rate) {
      const size_t learn_round = 0; // (1u << learn_rate) / 2;
      for (size_t i = 0; i < kWeights; ++i) {
        if (false && error) {
          const int32_t round = (1 << learn_rate) / 2;
          int64_t new_w = w[i] + ((s[i] << kShift / error + round) >> learn_rate);
          w[i] = std::max(std::min(new_w, kMaxW), kMinW);
        }
        auto delta = (s[i] + learn_round) >> learn_rate;
        if (error > 0) {
          w[i] += delta;
        } else if (error < 0) {
          w[i] -= delta;
        }
        // w[i] += error >> learn_rate;
      }
    }
  };

  Wav16() : opt_var(0) {
  }

  bool setOpt(uint32_t var) {
    opt_var = var;
    return true;
  }

  void init() {
    noise_bits_ = 3;
    non_noise_bits_ = 16 - noise_bits_;
    size_t num_ctx = 2 << (non_noise_bits_ + kContextBits);
    models_.resize(num_ctx);
    for (auto& m : models_) {
      m.init();
    }
  }

  template <const bool kDecode, typename TStream>
  uint32_t processSample(TStream& stream, size_t context, size_t channel, uint32_t c = 0) {
    uint32_t code = 0;
    if (!kDecode) {
      code = c << (sizeof(uint32_t) * 8 - 16);
    }
    int ctx = 1;
    context = context * 2 + channel;
    context <<= non_noise_bits_;
    check(context < models_.size());
    for (uint32_t i = 0; i < non_noise_bits_; ++i) {
      auto& m = models_[context + ctx];
      int p = m.getP();
      p += p == 0;
      uint32_t bit;
      if (kDecode) {
        bit = ent.getDecodedBit(p, kShift);
      } else {
        bit = code >> (sizeof(uint32_t) * 8 - 1);
        code <<= 1;
        ent.encode(stream, bit, p, kShift);
      }
      m.update(bit);
      ctx = ctx * 2 + bit;
      // Encode the bit / decode at the last second.
      if (kDecode) {
        ent.Normalize(stream);
      }
    }

    // Decode noisy bits (direct).
    for (size_t i = 0; i < noise_bits_; ++i) {
      if (kDecode) {
        ctx += ctx + ent.decodeBit(stream);
      } else {
        ent.encodeBit(stream, code >> 31); code <<= 1;
      }
    }

    return ctx ^ (1u << 16);
  }

  virtual void compress(Stream* in_stream, Stream* out_stream, uint64_t max_count) {
    BufferedStreamReader<4 * KB> sin(in_stream);
    BufferedStreamWriter<4 * KB> sout(out_stream);
    assert(in_stream != nullptr);
    assert(out_stream != nullptr);
    init();
    ent = Range7();
    uint16_t last_a = 0, last_b = 0;
    uint16_t last_a2 = 0, last_b2 = 0;
    uint16_t last_a3 = 0, last_b3 = 0;
    uint64_t i = 0;
    LinearMixer mix[2];
    uint64_t total_error = 0;
    for (; i < max_count; i += 4) {
      int c1 = sin.get(), c2 = sin.get();
      uint16_t a = c1 + c2 * 256;
      int c3 = sin.get(), c4 = sin.get();
      uint16_t b = c3 + c4 * 256;
      uint16_t pred_a = 2 * last_a - last_a2;
      uint16_t pred_b = 2 * last_b - last_b2;
      int32_t s0[] = { 2 * last_a - last_a2, last_a, last_a3, last_b };
      int32_t s1[] = { 2 * last_b - last_b2, last_b, a, last_a };
      //int32_t pred_a = mix[0].mix(s0);
      //int32_t pred_b = mix[1].mix(s1);
      int32_t error_a = static_cast<int32_t>(a) - pred_a;
      int32_t error_b = static_cast<int32_t>(b) - pred_b;
      total_error += std::abs(error_a) + std::abs(error_b);
      if (c1 == EOF) {
        break;
      }
      processSample<false>(sout, 0, 0, static_cast<uint16_t>(error_a));
      processSample<false>(sout, 0, 1, static_cast<uint16_t>(error_b));
      mix[0].update(s0, pred_a, error_a, 13);
      mix[1].update(s1, pred_b, error_b, 13);
      last_a3 = last_a2;
      last_b3 = last_b2;
      last_a2 = last_a;
      last_b2 = last_b;
      last_a = a;
      last_b = b;
    }
    std::cout << std::endl;
    mix[0].printWeights();
    std::cout << " / ";
    mix[1].printWeights();
    std::cout << "total error=" << total_error / 1000000.0 << std::endl;
    ent.flush(sout);
    sout.flush();
  }

  virtual void decompress(Stream* in_stream, Stream* out_stream, uint64_t max_count) {
    BufferedStreamReader<4 * KB> sin(in_stream);
    BufferedStreamWriter<4 * KB> sout(out_stream);
    auto start = in_stream->tell();
    init();
    ent.initDecoder(sin);
    uint16_t last_a = 0, last_b = 0;
    uint16_t last_a2 = 0, last_b2 = 0;
    uint16_t last_a3 = 0, last_b3 = 0;
    while (max_count > 0) {
      uint16_t pred_a = 2 * last_a - last_a2;
      uint16_t pred_b = 2 * last_b - last_b2;
      uint16_t a = pred_a + processSample<true>(sin, 0, 0);
      uint16_t b = pred_b + processSample<true>(sin, 0, 1);
      if (max_count > 0) { --max_count; sout.put(a & 0xFF); }
      if (max_count > 0) { --max_count; sout.put(a >> 8); }
      if (max_count > 0) { --max_count; sout.put(b & 0xFF); }
      if (max_count > 0) { --max_count; sout.put(b >> 8); }
      last_a3 = last_a2;
      last_b3 = last_b2;
      last_a2 = last_a;
      last_b2 = last_b;
      last_a = a;
      last_b = b;
    }
    sout.flush();
    size_t remain = sin.remain();
    if (remain > 0) {
      // Go back all the characters we didn't actually read.
      auto target = in_stream->tell() - remain;
      in_stream->seek(target);
    }
  }
};


#endif
