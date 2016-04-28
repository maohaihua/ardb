/*
 *Copyright (c) 2013-2014, yinqiwen <yinqiwen@gmail.com>
 *All rights reserved.
 *
 *Redistribution and use in source and binary forms, with or without
 *modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of Redis nor the names of its contributors may be used
 *    to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 *THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 *BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 *THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "codec.hpp"
#include "buffer/buffer_helper.hpp"
#include "util/murmur3.h"
#include "channel/all_includes.hpp"
#include <cmath>
#include <float.h>

OP_NAMESPACE_BEGIN

    void KeyObject::SetType(uint8 t)
    {
        type = t;
        switch (type)
        {
            case KEY_SET_MEMBER:
            case KEY_LIST_ELEMENT:
            case KEY_ZSET_SCORE:
            case KEY_HASH_FIELD:
            {
                elements.resize(1);
                break;
            }
            case KEY_ZSET_SORT:
            {
                elements.resize(2);
                break;
            }
            default:
            {
                break;
            }
        }
    }

    bool KeyObject::DecodeNS(Buffer& buffer, bool clone_str)
    {
        return ns.Decode(buffer, clone_str);
    }

    int KeyObject::Compare(const KeyObject& other) const
    {
        int ret = ns.Compare(other.ns, false);
        if (ret != 0)
        {
            return ret;
        }
        ret = key.Compare(other.key, false);
        if (ret != 0)
        {
            return ret;
        }
        ret = (int) type - (int) other.type;
        if (ret != 0)
        {
            return ret;
        }
        ret = elements.size() - other.elements.size();
        if (ret != 0)
        {
            return ret;
        }
        for (size_t i = 0; i < elements.size(); i++)
        {
            ret = elements[i].Compare(other.elements[i], false);
            if (ret != 0)
            {
                return ret;
            }
        }
        return 0;
    }

//    bool KeyObject::DecodeType(Buffer& buffer)
//    {
//        char tmp;
//        if (!buffer.ReadByte(tmp))
//        {
//            return false;
//        }
//        type = (uint8) tmp;
//        return true;
//    }
//    bool KeyObject::DecodeKey(Buffer& buffer, bool clone_str)
//    {
//        return key.Decode(buffer, clone_str);
//    }

    bool KeyObject::DecodeKey(Buffer& buffer, bool clone_str)
    {
        uint32 keylen;
        if (!BufferHelper::ReadVarUInt32(buffer, keylen))
        {
            return false;
        }
        if (buffer.ReadableBytes() < (keylen))
        {
            return false;
        }
        key.SetString(buffer.GetRawReadBuffer(), keylen, clone_str);
        buffer.AdvanceReadIndex(keylen);
        return true;
    }

    bool KeyObject::DecodeType(Buffer& buffer)
    {
        if (!buffer.Readable())
        {
            return false;
        }
        type = (uint8) (buffer.GetRawReadBuffer()[0]);
        buffer.AdvanceReadIndex(1);
        return true;
    }

    bool KeyObject::DecodePrefix(Buffer& buffer, bool clone_str)
    {
        if (!DecodeKey(buffer, clone_str))
        {
            return false;
        }
        return DecodeType(buffer);
    }

    int KeyObject::DecodeElementLength(Buffer& buffer)
    {
        char len;
        if (!buffer.ReadByte(len))
        {
            return 0;
        }
        if (len < 0 || len > 127)
        {
            return -1;
        }
        if (len > 0)
        {
            elements.resize(len);
        }
        return (int) len;
    }
    bool KeyObject::DecodeElement(Buffer& buffer, bool clone_str, int idx)
    {
        if (elements.size() <= idx)
        {
            elements.resize(idx + 1);
        }
        return elements[idx].Decode(buffer, clone_str);
    }
    bool KeyObject::Decode(Buffer& buffer, bool clone_str)
    {
        Clear();
        if (!DecodePrefix(buffer, clone_str))
        {
            return false;
        }
//        if (!DecodeKey(buffer, clone_str))
//        {
//            return false;
//        }
        int elen1 = DecodeElementLength(buffer);
        if (elen1 > 0)
        {
            for (int i = 0; i < elen1; i++)
            {
                if (!DecodeElement(buffer, clone_str, i))
                {
                    return false;
                }
            }
        }
        return true;
    }

    void KeyObject::EncodePrefix(Buffer& buffer) const
    {
        BufferHelper::WriteVarUInt32(buffer, key.StringLength());
        buffer.Write(key.CStr(), key.StringLength());
        buffer.WriteByte((char) type);
    }
    Slice KeyObject::Encode(Buffer& buffer, bool verify) const
    {
        if (verify && !IsValid())
        {
            return Slice();
        }
        size_t mark = buffer.GetWriteIndex();
        EncodePrefix(buffer);
        buffer.WriteByte((char) elements.size());
        for (size_t i = 0; i < elements.size(); i++)
        {
            elements[i].Encode(buffer);
        }
        return Slice(buffer.GetRawBuffer() + mark, buffer.GetWriteIndex() - mark);
    }
    void KeyObject::CloneStringPart()
    {
        if (ns.IsString())
        {
            ns.ToMutableStr();
        }
        if (key.IsString())
        {
            key.ToMutableStr();
        }
        for (size_t i = 0; i < elements.size(); i++)
        {
            if (elements[i].IsString())
            {
                elements[i].ToMutableStr();
            }
        }
    }
    bool KeyObject::IsValid() const
    {
        switch (type)
        {
            case KEY_META:
            case KEY_STRING:
            case KEY_HASH:
            case KEY_LIST:
            case KEY_SET:
            case KEY_ZSET:
            case KEY_HASH_FIELD:
            case KEY_LIST_ELEMENT:
            case KEY_SET_MEMBER:
            case KEY_ZSET_SORT:
            case KEY_ZSET_SCORE:
            {
                return true;
            }
            default:
            {
                return false;
            }
        }
    }

    Meta& ValueObject::GetMeta()
    {
        Data& meta = getElement(0);
        size_t reserved_size = sizeof(Meta);
        switch (type)
        {
            case KEY_LIST:
            {
                reserved_size = sizeof(ListMeta);
                break;
            }
            case KEY_HASH:
            case KEY_SET:
            case KEY_ZSET:
            {
                reserved_size = sizeof(MKeyMeta);
                break;
            }
            case KEY_STRING:
            {
                break;
            }
            default:
            {
                FATAL_LOG("Invalid type:%d to get ttl", type);
            }
        }
        return *(Meta*) meta.ReserveStringSpace(reserved_size);
    }

    MKeyMeta& ValueObject::GetMKeyMeta()
    {
        return (MKeyMeta&) GetMeta();
    }
    ListMeta& ValueObject::GetListMeta()
    {
        return (ListMeta&) GetMeta();
    }
    HashMeta& ValueObject::GetHashMeta()
    {
        return GetMKeyMeta();
    }
    SetMeta& ValueObject::GetSetMeta()
    {
        return GetMKeyMeta();
    }
    ZSetMeta& ValueObject::GetZSetMeta()
    {
        return GetMKeyMeta();
    }

    int64_t ValueObject::GetTTL()
    {
        return GetMeta().ttl;
    }
    void ValueObject::SetTTL(int64_t v)
    {
        GetMeta().ttl = v;
    }
    void ValueObject::ClearMinMaxData()
    {
        GetMin().Clear();
        GetMax().Clear();
    }
    bool ValueObject::SetMinData(const Data& v, bool overwite)
    {
        bool replaced = false;
        if (vals.size() < 3)
        {
            vals.resize(3);
            replaced = true;
        }
        if (overwite || vals[1] > v || vals[1].IsNil())
        {
            vals[1] = v;
            replaced = true;
        }
        return replaced;
    }
    bool ValueObject::SetMaxData(const Data& v, bool overwite)
    {
        bool replaced = false;
        if (vals.size() < 3)
        {
            vals.resize(3);
            replaced = true;
        }
        if (overwite || vals[2] < v || vals[2].IsNil())
        {
            vals[2] = v;
            replaced = true;
        }
        return replaced;
    }
    bool ValueObject::SetMinMaxData(const Data& v)
    {
        bool replaced = false;
        if (vals.size() < 3)
        {
            vals.resize(3);
            replaced = true;
        }
        if (vals[2].IsNil() && vals[1].IsNil())
        {
            vals[1] = v;
            vals[2] = v;
        }
        else
        {
            if (vals[1] > v || vals[1].IsNil())
            {
                vals[1] = v;
                replaced = true;
            }
            if (vals[2] < v)
            {
                vals[2] = v;
                replaced = true;
            }
        }
        return replaced;
    }

    static void encode_value_object(Buffer& encode_buffer, uint8 type, uint16 merge_op, const DataArray& args)
    {
        encode_buffer.WriteByte((char) type);
        switch (type)
        {
            case KEY_MERGE:
            {
                BufferHelper::WriteFixUInt16(encode_buffer, merge_op, true);
                break;
            }
            default:
            {
                break;
            }
        }
        encode_buffer.WriteByte((char) args.size());
        for (size_t i = 0; i < args.size(); i++)
        {
            args[i].Encode(encode_buffer);
        }
    }

    void ValueObject::SetType(uint8 t)
    {
        type = t;
        switch (type)
        {
            case KEY_STRING:
            case KEY_HASH:
            case KEY_LIST:
            case KEY_SET:
            case KEY_ZSET:
            {
                GetMeta();
                break;
            }
            default:
            {
                break;
            }
        }
    }

    Slice ValueObject::Encode(Buffer& encode_buffer) const
    {
        if (0 == type)
        {
            return Slice();
        }
        encode_value_object(encode_buffer, type, merge_op, vals);
        return Slice(encode_buffer.GetRawReadBuffer(), encode_buffer.ReadableBytes());
    }

    bool ValueObject::DecodeMeta(Buffer& buffer, bool clone_str)
    {
        Clear();
        if (!buffer.Readable())
        {
            return true;
        }
        char tmp;
        if (!buffer.ReadByte(tmp))
        {
            return false;
        }
        type = (uint8) tmp;
        if (type == KEY_MERGE)
        {
            if (!BufferHelper::ReadFixUInt16(buffer, merge_op, true))
            {
                return false;
            }
        }
        char lench;
        if (!buffer.ReadByte(lench))
        {
            return false;
        }
        uint8 len = (uint8) lench;
        if (len > 0)
        {
            /*
             * Meta value is the first element
             */
            return vals[0].Decode(buffer, clone_str);
        }
        return false;
    }

    bool ValueObject::Decode(Buffer& buffer, bool clone_str)
    {
        Clear();
        if (!buffer.Readable())
        {
            return true;
        }
        char tmp;
        if (!buffer.ReadByte(tmp))
        {
            return false;
        }
        type = (uint8) tmp;
        if (type == KEY_MERGE)
        {
            if (!BufferHelper::ReadFixUInt16(buffer, merge_op, true))
            {
                return false;
            }
        }
        char lench;
        if (!buffer.ReadByte(lench))
        {
            return false;
        }
        uint8 len = (uint8) lench;
        if (len > 0)
        {
            vals.resize(len);
            for (uint8 i = 0; i < len; i++)
            {
                if (!vals[i].Decode(buffer, clone_str))
                {
                    return false;
                }
            }
        }
        return true;
    }

    static int parse_score(const std::string& score_str, double& score, bool& contain)
    {
        contain = true;
        const char* str = score_str.c_str();
        if (score_str.at(0) == '(')
        {
            contain = false;
            str++;
        }
        if (strcasecmp(str, "-inf") == 0)
        {
            score = -DBL_MAX;
        }
        else if (strcasecmp(str, "+inf") == 0)
        {
            score = DBL_MAX;
        }
        else
        {
            if (!str_todouble(str, score))
            {
                return -1;
            }
        }
        return 0;
    }
    bool ZRangeSpec::Parse(const std::string& minstr, const std::string& maxstr)
    {
        double min_d, max_d;
        int ret = parse_score(minstr, min_d, contain_min);
        if (0 == ret)
        {
            ret = parse_score(maxstr, max_d, contain_max);
        }
        if (ret == 0 && min_d > max_d)
        {
            return false;
        }
        min.SetFloat64(min_d);
        max.SetFloat64(max_d);
        return ret == 0;
    }
    static bool verify_lexrange_para(const std::string& str)
    {
        if (str == "-" || str == "+")
        {
            return true;
        }
        if (str.empty())
        {
            return false;
        }

        if (str[0] != '(' && str[0] != '[')
        {
            return false;
        }
        return true;
    }
    bool ZLexRangeSpec::Parse(const std::string& minstr, const std::string& maxstr)
    {
        if (!verify_lexrange_para(minstr) || !verify_lexrange_para(maxstr))
        {
            return false;
        }
        if (minstr[0] == '(')
        {
            include_min = false;
        }
        if (minstr[0] == '[')
        {
            include_min = true;
        }
        if (maxstr[0] == '(')
        {
            include_max = false;
        }
        if (maxstr[0] == '[')
        {
            include_max = true;
        }
        if (minstr == "-")
        {
            min.clear();
            include_min = true;
        }
        else
        {
            min = minstr.substr(1);
        }
        if (maxstr == "+")
        {
            max.clear();
            include_max = true;
        }
        else
        {
            max = maxstr.substr(1);
        }
        if (min > max && !max.empty())
        {
            return false;
        }
        return true;
    }

    int encode_merge_operation(Buffer& buffer, uint16_t op, const DataArray& args)
    {
        encode_value_object(buffer, KEY_MERGE, op, args);
        return 0;
    }

    KeyType element_type(KeyType type)
    {
        switch (type)
        {
            case KEY_HASH:
            {
                return KEY_HASH_FIELD;
            }
            case KEY_LIST:
            {
                return KEY_LIST_ELEMENT;
            }
            case KEY_SET:
            {
                return KEY_SET_MEMBER;
            }
            case KEY_ZSET:
            {
                return KEY_ZSET_SCORE;
            }
            default:
            {
                abort();
            }
        }
    }

OP_NAMESPACE_END

