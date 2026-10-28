/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef OCEANBASE_OB_DATUM_CMP_FUNC_DEF_H
#define OCEANBASE_OB_DATUM_CMP_FUNC_DEF_H

#include <type_traits>

#include "ob_defined_collations.h"

#include "common/object/ob_obj_type.h"
#include "lib/charset/ob_charset.h"
#include "common/number/ob_number_v2.h"
#include "common/timezone/ob_timezone_info.h"
#include "common/datum/ob_datum.h"
#include "ob_datum_util.h"
#include "common/json_type/ob_json_base.h" // for ObIJsonBase
#include "common/json_type/ob_json_bin.h" // for ObJsonBin
#include "common/wide_integer/ob_wide_integer.h"
#include "share/ob_errno.h"

namespace oceanbase
{
namespace common
{
struct ObDatumAccessContext;
namespace datum_cmp
{

// We define three core comparison:
//
// 1. ObDatumTypeCmp<ObObjType, ObObjType>: define non string obj type compare,
//    if comparison not deinfed for specified type, use ObDatumTCCmp defined comparison.
// 2. ObDatumTCCmp<ObObjTypeClass, ObObjTypeClass>: define non string obj type class compare
// 3. ObDatumStrCmp<ObCollationType, bool>: define string compare

template <bool V = true>
struct ObDefined
{
  constexpr static bool defined_ = V;
};

// define compare by ObObjTypeClass
template <ObObjTypeClass L_TC, ObObjTypeClass R_TC>
struct ObDatumTCCmp : public ObDefined<false>
{
  inline static int cmp(const ObDatum &, const ObDatum &, int &cmp_ret)
  {
    cmp_ret = 0;
    return OB_SUCCESS;
  }
};

template <ObObjTypeClass TC>
struct ObTCPayloadCmp : public ObDefined<>
{
  inline static int cmp(const ObDatum &l, const ObDatum &r, int &cmp_ret)
  {
    cmp_ret = ObDatumPayload<TC>::get(l) == ObDatumPayload<TC>::get(r)
              ? 0
              : (ObDatumPayload<TC>::get(l) < ObDatumPayload<TC>::get(r) ? -1 : 1);
    return OB_SUCCESS;
  }
};

// cmp(signed, unsgined)
struct ObSignedUnsignedCmp : public ObDefined<>
{
  inline static int cmp(const ObDatum &l, const ObDatum &r, int &cmp_ret)
  {
    cmp_ret = l.get_int() < 0
              ? -1
              : (l.get_int() < r.get_uint()
                ? -1
                : (l.get_int() == r.get_uint() ? 0 : 1));
    return OB_SUCCESS;
  }
};

// cmp(unsigned, signed)
struct ObUnsignedSignedCmp : public ObDefined<>
{
  inline static int cmp(const ObDatum &l, const ObDatum &r, int &cmp_ret)
  {
    int ret = ObSignedUnsignedCmp::cmp(r, l, cmp_ret);
    cmp_ret = -cmp_ret;
    return ret;
  }
};

// specialization for all type class.
template <> struct ObDatumTCCmp<ObIntTC, ObIntTC> : public ObTCPayloadCmp<ObIntTC> {};
template <> struct ObDatumTCCmp<ObUIntTC, ObUIntTC> : public ObTCPayloadCmp<ObUIntTC> {};
template <>
struct ObDatumTCCmp<ObFloatTC, ObFloatTC> : public ObDefined<>
{
  template <typename T>
  inline static int real_value_cmp(T l, T r, int &cmp_ret)
  {
    cmp_ret = 0;
    // Note: For NaN, we can't use C language compare logic.
    // NaN sorts after any numeric value.
    if (isnan(l) || isnan(r)) {
      if (isnan(l) && isnan(r)) {
        cmp_ret = 0;
      } else if (isnan(l)) {
        // l is nan, r is not nan:left always bigger than right
        cmp_ret = 1;
      } else {
        // l is not nan, r is nan, left always less than right
        cmp_ret = -1;
      }
    } else {
      cmp_ret = l == r ? 0 : (l < r ? -1 : 1);
    }
    return OB_SUCCESS;
  }

  inline static int cmp(const ObDatum &l, const ObDatum &r, int &cmp_ret)
  {
    return real_value_cmp(l.get_float(), r.get_float(), cmp_ret);
  }
};

template <ObScale SCALE>
struct ObFixedDoubleCmp: public ObDefined<>
{
  constexpr static double LOG_10[32] =
  {
    1e000, 1e001, 1e002, 1e003, 1e004, 1e005, 1e006, 1e007,
    1e008, 1e009, 1e010, 1e011, 1e012, 1e013, 1e014, 1e015,
    1e016, 1e017, 1e018, 1e019, 1e020, 1e021, 1e022, 1e023,
    1e024, 1e025, 1e026, 1e027, 1e028, 1e029, 1e030, 1e031
  };
  constexpr static double P = 5 / LOG_10[SCALE + 1];
  inline static int cmp(const ObDatum &l_datum, const ObDatum &r_datum, int &cmp_ret)
  {
    cmp_ret = 0;
    const double l = l_datum.get_double();
    const double r = r_datum.get_double();
    if (isnan(l) || isnan(r)) {
      if (isnan(l) && isnan(r)) {
        cmp_ret = 0;
      } else if (isnan(l)) {
        // l is nan, r is not nan:left always bigger than right
        cmp_ret = 1;
      } else {
        // l is not nan, r is nan, left always less than right
        cmp_ret = -1;
      }
    } else if (l == r || fabs(l - r) < P) {
      cmp_ret = 0;
    } else {
      cmp_ret = (l < r ? -1 : 1);
    }
    return OB_SUCCESS;
  }
};


template <>
struct ObDatumTCCmp<ObDoubleTC, ObDoubleTC> : public ObDatumTCCmp<ObFloatTC, ObFloatTC>
{
  inline static int cmp(const ObDatum &l, const ObDatum &r, int &cmp_ret)
  {
    return ObDatumTCCmp<ObFloatTC, ObFloatTC>::real_value_cmp(l.get_double(), r.get_double(), cmp_ret);
  }
};

template <>
struct ObDatumTCCmp<ObNumberTC, ObNumberTC> : public ObDefined<>
{
  inline static int cmp(const ObDatum &l, const ObDatum &r, int &cmp_ret)
  {
    cmp_ret = number::ObNumber::compare(l.get_number_desc(), l.get_number_digits(),
                                        r.get_number_desc(), r.get_number_digits());
    return OB_SUCCESS;
  }
};

template<>
struct ObDatumTCCmp<ObDecimalIntTC, ObDecimalIntTC>: public ObDefined<>
{
  inline static int cmp(const ObDatum &l, const ObDatum &r, int &cmp_ret)
  {
    int ret = OB_SUCCESS;
    if (OB_FAIL(wide::compare(l, r, cmp_ret))) {
    }
    return ret;
  }
};

template<ObDecimalIntWideType lw, ObDecimalIntWideType rw>
struct ObDecintCmp: public ObDefined<>
{
  static_assert(lw < DECIMAL_INT_MAX && rw < DECIMAL_INT_MAX, "");
  inline static int cmp(const ObDatum &l, const ObDatum &r, int &cmp_ret)
  {
    decint_cmp_fp cmp =
      wide::ObDecimalIntCmpSet::decint_decint_cmp_set_[(2 << (lw + 1))][(2 << (rw + 1))];
    cmp_ret = cmp(l.get_decimal_int(), r.get_decimal_int());
    return OB_SUCCESS;
  }
};

template <> struct ObDatumTCCmp<ObDateTC, ObDateTC> : public ObTCPayloadCmp<ObDateTC> {};
template <> struct ObDatumTCCmp<ObTimeTC, ObTimeTC> : public ObTCPayloadCmp<ObTimeTC> {};
template <> struct ObDatumTCCmp<ObYearTC, ObYearTC> : public ObTCPayloadCmp<ObYearTC> {};
template <> struct ObDatumTCCmp<ObBitTC, ObBitTC> : public ObTCPayloadCmp<ObBitTC> {};
template <> struct ObDatumTCCmp<ObEnumSetTC, ObEnumSetTC> : public ObTCPayloadCmp<ObEnumSetTC> {};

// different type class compare
template <> struct ObDatumTCCmp<ObIntTC, ObUIntTC> : public ObSignedUnsignedCmp {};
template <> struct ObDatumTCCmp<ObIntTC, ObEnumSetTC> : public ObSignedUnsignedCmp {};
template <> struct ObDatumTCCmp<ObUIntTC, ObIntTC> : public ObUnsignedSignedCmp {};
template <> struct ObDatumTCCmp<ObEnumSetTC, ObIntTC> : public ObUnsignedSignedCmp {};
template <> struct ObDatumTCCmp<ObUIntTC, ObEnumSetTC> : public ObTCPayloadCmp<ObUIntTC> {};
template <> struct ObDatumTCCmp<ObEnumSetTC, ObUIntTC> : public ObTCPayloadCmp<ObUIntTC> {};
template <>
struct ObDatumTCCmp<ObMySQLDateTC, ObMySQLDateTC> : public ObTCPayloadCmp<ObMySQLDateTC> {};
template <>
struct ObDatumTCCmp<ObMySQLDateTimeTC, ObMySQLDateTimeTC> : public ObTCPayloadCmp<ObMySQLDateTimeTC> {};

// special process for extend and null class type.

// extend type vs any type is depend on extend type is min or max
template <ObObjTypeClass TC>
struct ObDatumTCCmp<ObExtendTC, TC> : public ObDefined<>
{
  inline static int cmp(const ObDatum &l, const ObDatum &, int &cmp_ret)
  {
    cmp_ret = (ObObj::MIN_OBJECT_VALUE == *l.int_ ? -1 : 1);
    return OB_SUCCESS;
  }
};

template <ObObjTypeClass TC>
struct ObDatumTCCmp<TC, ObExtendTC> : public ObDefined<>
{
  inline static int cmp(const ObDatum &, const ObDatum &r, int &cmp_ret)
  {
    cmp_ret = (ObObj::MIN_OBJECT_VALUE == *r.int_ ? -1 : 1);
    return OB_SUCCESS;
  }
};

template <>
struct ObDatumTCCmp<ObExtendTC, ObExtendTC> : public ObDefined<>
{
  inline static int cmp(const ObDatum &l, const ObDatum &r, int &cmp_ret)
  {
    cmp_ret = (ObObj::MIN_OBJECT_VALUE == *l.int_ && ObObj::MIN_OBJECT_VALUE == *r.int_)
              || (ObObj::MAX_OBJECT_VALUE == *l.int_ && ObObj::MAX_OBJECT_VALUE == *r.int_)
              ? 0 : (ObObj::MIN_OBJECT_VALUE == *l.int_ ? -1 : 1);
    return OB_SUCCESS;
  }
};

struct ObDummyCmp : public ObDefined<>
{
  inline static int cmp(const ObDatum &, const ObDatum &, int &cmp_ret)
  {
    cmp_ret = 0;
    return OB_SUCCESS;
  }
};

// null type compare is never used (out layer guaranteed), but should be defined.
template <ObObjTypeClass TC> struct ObDatumTCCmp<TC, ObNullTC> : public ObDummyCmp {};
template <ObObjTypeClass TC> struct ObDatumTCCmp<ObNullTC, TC> : public ObDummyCmp {};
template <> struct ObDatumTCCmp<ObNullTC, ObExtendTC> : public ObDummyCmp {};
template <> struct ObDatumTCCmp<ObExtendTC, ObNullTC> : public ObDummyCmp {};
template <> struct ObDatumTCCmp<ObNullTC, ObNullTC> : public ObDummyCmp {};


///////////////////////////////////////////////////////////////////////////////
// begin define compare by ObObjType
///////////////////////////////////////////////////////////////////////////////

// define compare by ObObjType
template <ObObjType L_T, ObObjType R_T>
struct ObDatumTypeCmp : public ObDefined<false>
{
  inline static int cmp(const ObDatum &, const ObDatum &, int &cmp_ret)
  {
    cmp_ret = 0;
    return OB_SUCCESS;
  }
};

template <>
struct ObDatumTypeCmp<ObDateTimeType, ObDateTimeType> : public ObTCPayloadCmp<ObDateTimeTC> {};
template <>
struct ObDatumTypeCmp<ObTimestampType, ObTimestampType> : public ObTCPayloadCmp<ObDateTimeTC> {};

struct ObDatumJsonCmpImpl
{
  static int cmp(const ObDatum &l, const ObDatum &r, int &cmp_ret, const bool is_lob,
                 const ObDatumAccessContext *access_ctx);
};

template <bool HAS_LOB_LOCATOR>
struct ObDatumJsonCmp : public ObDefined<>
{
  inline static int cmp(const ObDatum &l, const ObDatum &r, int &cmp_ret,
                        const ObDatumAccessContext *access_ctx)
  {
    return ObDatumJsonCmpImpl::cmp(l, r, cmp_ret, HAS_LOB_LOCATOR, access_ctx);
  }
};

struct ObDatumGeoCmpImpl
{
  static int cmp(const ObDatum &l, const ObDatum &r, int &cmp_ret, const bool is_lob,
                 const ObDatumAccessContext *access_ctx);
};

template <bool HAS_LOB_HEADER>
struct ObDatumGeoCmp : public ObDefined<>
{
  inline static int cmp(const ObDatum &l, const ObDatum &r, int &cmp_ret,
                        const ObDatumAccessContext *access_ctx)
  {
    return ObDatumGeoCmpImpl::cmp(l, r, cmp_ret, HAS_LOB_HEADER, access_ctx);
  }
};

template <bool HAS_LOB_HEADER>
struct ObDatumUDTCmp : public ObDefined<>
{
  inline static int cmp(const ObDatum &l, const ObDatum &r, int &cmp_ret)
  {
    UNUSED(l);
    UNUSED(r);
    UNUSED(cmp_ret);
    return OB_ERR_INVALID_TYPE_FOR_OP;
  }
};

struct ObDatumCollectionCmpImpl
{
  static int cmp(const ObDatum &l, const ObDatum &r, int &cmp_ret, const bool is_lob,
                 const ObDatumAccessContext *access_ctx);
};

template <bool HAS_LOB_HEADER>
struct ObDatumCollectionCmp : public ObDefined<>
{
  inline static int cmp(const ObDatum &l, const ObDatum &r, int &cmp_ret,
                        const ObDatumAccessContext *access_ctx)
  {
    return ObDatumCollectionCmpImpl::cmp(l, r, cmp_ret, HAS_LOB_HEADER, access_ctx);
  }
};

///////////////////////////////////////////////////////////////////////////////
// begin define string compare functions
///////////////////////////////////////////////////////////////////////////////


// utf8mb4_general_ci charset info and the sort weights of its ASCII characters, resolved once
// so that hot comparisons skip the argument checks and charset lookup of ObCharset::strcmpsp.
struct Utf8mb4GeneralCiCmpCtx
{
  Utf8mb4GeneralCiCmpCtx();
  static const Utf8mb4GeneralCiCmpCtx &get_instance()
  {
    static Utf8mb4GeneralCiCmpCtx ctx;
    return ctx;
  }
  // Decodes a character of at most 3 bytes like ob_mb_wc_utf8mb4 and gets its sort weight like
  // ob_tosort_unicode. Returns false for longer characters and invalid or truncated sequences.
  OB_INLINE bool get_bmp_weight(const unsigned char *s, const uint32_t len,
                                uint32_t &weight, uint32_t &char_len) const
  {
    bool valid = true;
    uint32_t wc = 0;
    const unsigned char c = s[0];
    if (c < 0x80) {
      weight = ascii_weight_[c];
      char_len = 1;
    } else if (c < 0xc2 || c >= 0xf0) {
      valid = false;
    } else if (c < 0xe0) {
      if (len < 2 || (s[1] >> 6) != 0x02) {
        valid = false;
      } else {
        wc = ((c & 0x1f) << 6) | (s[1] ^ 0x80);
        char_len = 2;
      }
    } else if (len < 3 || (s[1] >> 6) != 0x02 || (s[2] >> 6) != 0x02 || (c < 0xe1 && s[1] < 0xa0)) {
      valid = false;
    } else {
      wc = ((c & 0x0f) << 12) | ((s[1] ^ 0x80) << 6) | (s[2] ^ 0x80);
      char_len = 3;
    }
    if (valid && c >= 0x80) {
      if (wc > maxchar_) {
        weight = OB_CS_REPLACEMENT_CHARACTER;
      } else {
        const ObUnicaseInfoChar *page = uni_plane_->page[wc >> 8];
        weight = OB_ISNULL(page) ? wc : (lower_sort_ ? page[wc & 0xFF].tolower : page[wc & 0xFF].sort);
      }
    }
    return valid;
  }
  const ObCharsetInfo *cs_;
  const ObUnicaseInfo *uni_plane_;
  ob_wc_t maxchar_;
  bool lower_sort_;
  uint32_t ascii_weight_[128];
};

struct ObDatumStrCmpImpl
{
  static int OB_INLINE cmp(const ObDatum &l, const ObDatum &r, int &cmp_ret,
                           const ObCollationType cs, const bool with_end_space)
  {
    if (OB_UNLIKELY((nullptr == l.ptr_ && 0 != l.len_) || (nullptr == r.ptr_ && 0 != r.len_))) {
      // let ObCharset report the invalid argument
      cmp_ret = ObCharset::strcmpsp(cs, l.ptr_, l.len_, r.ptr_, r.len_, with_end_space);
    } else if (CS_TYPE_BINARY == cs) {
      cmp_ret = binary_cmp(l, r);
    } else if (CS_TYPE_UTF8MB4_GENERAL_CI == cs) {
      cmp_ret = utf8mb4_general_ci_cmp(l, r, with_end_space);
    } else {
      cmp_ret = ObCharset::strcmpsp(cs, l.ptr_, l.len_, r.ptr_, r.len_, with_end_space);
    }
    cmp_ret = cmp_ret > 0 ? 1 : (cmp_ret < 0 ? -1 : 0);
    return OB_SUCCESS;
  }

private:
  // Unaligned load of 8 bytes as a big endian integer, so that integer order is memcmp order.
  static OB_INLINE uint64_t load_be64(const char *p)
  {
    uint64_t v = 0;
    __builtin_memcpy(&v, p, sizeof(v));
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    v = __builtin_bswap64(v);
#endif
    return v;
  }

  // same sign as memcmp, compares short strings without a library call
  static OB_INLINE int bytes_cmp(const char *l, const char *r, const uint32_t len)
  {
    int res = 0;
    uint32_t pos = 0;
    for (; 0 == res && pos + sizeof(uint64_t) <= len; pos += sizeof(uint64_t)) {
      const uint64_t lv = load_be64(l + pos);
      const uint64_t rv = load_be64(r + pos);
      if (lv != rv) {
        res = lv < rv ? -1 : 1;
      }
    }
    for (; 0 == res && pos < len; ++pos) {
      const unsigned char lc = static_cast<unsigned char>(l[pos]);
      const unsigned char rc = static_cast<unsigned char>(r[pos]);
      if (lc != rc) {
        res = lc < rc ? -1 : 1;
      }
    }
    return res;
  }

  // same result as ob_strnncollsp_binary
  static int OB_INLINE binary_cmp(const ObDatum &l, const ObDatum &r)
  {
    const uint32_t min_len = l.len_ < r.len_ ? l.len_ : r.len_;
    int res = bytes_cmp(l.ptr_, r.ptr_, min_len);
    if (0 == res) {
      res = static_cast<int>(static_cast<size_t>(l.len_) - static_cast<size_t>(r.len_));
    }
    return res;
  }

  // Same result as ob_strnncollsp_utf8mb4. That function compares character by character and
  // its result only depends on the remaining suffixes, so leading characters of at most 3 bytes
  // are compared here and the rest is handed to it.
  static int OB_INLINE utf8mb4_general_ci_cmp(const ObDatum &l, const ObDatum &r,
                                              const bool with_end_space)
  {
    int res = 0;
    const Utf8mb4GeneralCiCmpCtx &ctx = Utf8mb4GeneralCiCmpCtx::get_instance();
    if (OB_ISNULL(ctx.cs_)) {
      res = ObCharset::strcmpsp(CS_TYPE_UTF8MB4_GENERAL_CI, l.ptr_, l.len_, r.ptr_, r.len_,
                                with_end_space);
    } else if (l.len_ == r.len_ && 0 == bytes_cmp(l.ptr_, r.ptr_, l.len_)) {
      // identical bytes compare equal, including invalid sequences (compared as binary)
      res = 0;
    } else {
      const unsigned char *s = reinterpret_cast<const unsigned char *>(l.ptr_);
      const unsigned char *t = reinterpret_cast<const unsigned char *>(r.ptr_);
      const uint32_t s_len = l.len_;
      const uint32_t t_len = r.len_;
      uint32_t s_pos = 0;
      uint32_t t_pos = 0;
      bool decided = false;
      while (!decided && s_pos < s_len && t_pos < t_len) {
        uint32_t s_weight = 0;
        uint32_t t_weight = 0;
        uint32_t s_char_len = 0;
        uint32_t t_char_len = 0;
        if (!ctx.get_bmp_weight(s + s_pos, s_len - s_pos, s_weight, s_char_len)
            || !ctx.get_bmp_weight(t + t_pos, t_len - t_pos, t_weight, t_char_len)) {
          break;
        } else if (s_weight != t_weight) {
          res = s_weight > t_weight ? 1 : -1;
          decided = true;
        } else {
          s_pos += s_char_len;
          t_pos += t_char_len;
        }
      }
      if (!decided) {
        res = ctx.cs_->coll->strnncollsp(ctx.cs_, s + s_pos, s_len - s_pos, t + t_pos,
                                         t_len - t_pos, with_end_space);
      }
    }
    return res;
  }
};

template <ObCollationType CS_TYPE, bool WITH_END_SPACE>
struct ObDatumStrCmp : public ObDefined<SupportedCollection<CS_TYPE>::defined_>
{
  inline static int cmp(const ObDatum &l, const ObDatum &r, int &cmp_ret)
  {
    return ObDatumStrCmpImpl::cmp(l, r, cmp_ret, CS_TYPE, WITH_END_SPACE);
  }
};

struct ObDatumTextCmpImpl
{
  static OB_INLINE int cmp(const ObDatum &l, const ObDatum &r, int &cmp_ret,
                 const ObCollationType cs, const bool with_end_space,
                 const ObDatumAccessContext *access_ctx)
  {
    int ret = OB_SUCCESS;
    const ObLobCommon& rlob = r.get_lob_data();
    const ObLobCommon& llob = l.get_lob_data();
    if (r.len_ != 0 && !rlob.is_mem_loc_ && rlob.in_row_ &&
        l.len_ != 0 && !llob.is_mem_loc_ && llob.in_row_) {
        cmp_ret = ObCharset::strcmpsp(
            cs, llob.get_inrow_data_ptr(), static_cast<int32_t>(llob.get_byte_size(l.len_)),
            rlob.get_inrow_data_ptr(), static_cast<int32_t>(rlob.get_byte_size(r.len_)), with_end_space);
      cmp_ret = cmp_ret > 0 ? 1 : (cmp_ret < 0 ? -1 : 0);
    } else {
      ret = cmp_out_row(l, r, cmp_ret, cs, with_end_space, access_ctx);
    }
    return ret;
  }

private:
  static int cmp_out_row(const ObDatum &l, const ObDatum &r, int &cmp_ret,
                          const ObCollationType cs, const bool with_end_space,
                          const ObDatumAccessContext *access_ctx);
};

template <ObCollationType CS_TYPE, bool WITH_END_SPACE>
struct ObDatumTextCmp : public ObDefined<SupportedCollection<CS_TYPE>::defined_>
{
  inline static int cmp(const ObDatum &l, const ObDatum &r, int &cmp_ret,
                        const ObDatumAccessContext *access_ctx)
  {
    return ObDatumTextCmpImpl::cmp(
        l, r, cmp_ret, CS_TYPE, WITH_END_SPACE, access_ctx);
  }
};

struct ObDatumTextStringCmpImpl
{
  static OB_INLINE int cmp(const ObDatum &l, const ObDatum &r, int &cmp_ret,
                 const ObCollationType cs, const bool with_end_space,
                 const ObDatumAccessContext *access_ctx)
  {
    int ret = OB_SUCCESS;
    const ObLobCommon& llob = l.get_lob_data();
    if (l.len_ != 0 && !llob.is_mem_loc_ && llob.in_row_) {
      cmp_ret = ObCharset::strcmpsp(
          cs, llob.get_inrow_data_ptr(), static_cast<int32_t>(llob.get_byte_size(l.len_)),
          r.ptr_, r.len_, with_end_space);
      cmp_ret = cmp_ret > 0 ? 1 : (cmp_ret < 0 ? -1 : 0);
    } else {
      ret = cmp_out_row(l, r, cmp_ret, cs, with_end_space, access_ctx);
    }
    return ret;
  }

private:
  static int cmp_out_row(const ObDatum &l, const ObDatum &r, int &cmp_ret,
                          const ObCollationType cs, const bool with_end_space,
                          const ObDatumAccessContext *access_ctx);
};

template <ObCollationType CS_TYPE, bool WITH_END_SPACE>
struct ObDatumTextStringCmp : public ObDefined<SupportedCollection<CS_TYPE>::defined_>
{
  inline static int cmp(const ObDatum &l, const ObDatum &r, int &cmp_ret,
                        const ObDatumAccessContext *access_ctx)
  {
    return ObDatumTextStringCmpImpl::cmp(
        l, r, cmp_ret, CS_TYPE, WITH_END_SPACE, access_ctx);
  }
};

struct ObDatumStringTextCmpImpl
{
  static int cmp(const ObDatum &l, const ObDatum &r, int &cmp_ret,
                 const ObCollationType cs, const bool with_end_space,
                 const ObDatumAccessContext *access_ctx)
  {
    int ret = OB_SUCCESS;
    cmp_ret = 0;
    const ObLobCommon& rlob = r.get_lob_data();
    if (r.len_ != 0 && !rlob.is_mem_loc_ && rlob.in_row_ && rlob.reserve_ == 0) {
      cmp_ret = ObCharset::strcmpsp(
          cs, l.ptr_, l.len_,
          rlob.get_inrow_data_ptr(), static_cast<int32_t>(rlob.get_byte_size(r.len_)), with_end_space);
      cmp_ret = cmp_ret > 0 ? 1 : (cmp_ret < 0 ? -1 : 0);
    } else {
      ret = cmp_out_row(l, r, cmp_ret, cs, with_end_space, access_ctx);
    }
    return ret;
  }
private:
  static int cmp_out_row(const ObDatum &l, const ObDatum &r, int &cmp_ret,
                          const ObCollationType cs, const bool with_end_space,
                          const ObDatumAccessContext *access_ctx);
};

template <ObCollationType CS_TYPE, bool WITH_END_SPACE>
struct ObDatumStringTextCmp : public ObDefined<SupportedCollection<CS_TYPE>::defined_>
{
  inline static int cmp(const ObDatum &l, const ObDatum &r, int &cmp_ret,
                        const ObDatumAccessContext *access_ctx)
  {
    return ObDatumStringTextCmpImpl::cmp(
        l, r, cmp_ret, CS_TYPE, WITH_END_SPACE, access_ctx);
  }
};

} // end namespace datum_cmp
} // end namespace common
} // end namespace oceanbase
#endif // OCEANBASE_OB_DATUM_CMP_FUNC_DEF_H_
