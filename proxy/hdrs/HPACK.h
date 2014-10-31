/** @file

  A brief file description

  @section license License

  Licensed to the Apache Software Foundation (ASF) under one
  or more contributor license agreements.  See the NOTICE file
  distributed with this work for additional information
  regarding copyright ownership.  The ASF licenses this file
  to you under the Apache License, Version 2.0 (the
  "License"); you may not use this file except in compliance
  with the License.  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
 */

#ifndef __HPACK_H__
#define __HPACK_H__

#include "libts.h"
#include "HTTP.h"

enum HEADER_INDEXING_TYPE {
  INC_INDEXING,
  WITHOUT_INDEXING,
  NEVER_INDEXED,
};

class MIMEFieldWrapper
{
public:

  MIMEFieldWrapper(MIMEField * f, HdrHeap * hh, MIMEHdrImpl * impl)
    : _field(f), _heap(hh), _mh(impl) {
  }

  void name_set(const char * name, int name_len) {
    _field->name_set(_heap, _mh, name, name_len);
  }

  void value_set(const char * value, int value_len) {
    _field->value_set(_heap, _mh, value, value_len);
  }

  const char * name_get(int * length) const {
    return _field->name_get(length);
  }

  const char * value_get(int * length) const {
    return _field->value_get(length);
  }

  const MIMEField * field_get() const {
    return _field;
  }

private:
  MIMEField *   _field;
  HdrHeap *     _heap;
  MIMEHdrImpl * _mh;
};

// 3.2 Header Table
class Http2HeaderTable
{
public:

  Http2HeaderTable() : _current_size(0), _settings_header_table_size(4096) {
    _mhdr = new MIMEHdr();
    _mhdr->create();
  }

  ~Http2HeaderTable() {
    _headers.clear();
    _mhdr->fields_clear();
  }

  void add_header_field(const MIMEField * field);
  int get_header_from_indexing_tables(uint32_t index, MIMEFieldWrapper& header_field) const;
  void set_header_table_size(uint32_t new_size);

private:

  const MIMEField * get_header(uint32_t index) const {
    return _headers.get(index-1);
  }

  const uint32_t get_current_entry_num() const {
    return _headers.length();
  }

  uint32_t          _current_size;
  uint32_t          _settings_header_table_size;

  MIMEHdr *         _mhdr;
  Vec<MIMEField *>  _headers;
};

int64_t
encode_indexed_header_field(uint8_t *buf_start, const uint8_t *buf_end, uint32_t index);
int64_t
encode_literal_header_field(uint8_t *buf_start, const uint8_t *buf_end, const MIMEFieldWrapper& header, uint32_t index, HEADER_INDEXING_TYPE type);
int64_t
encode_literal_header_field(uint8_t *buf_start, const uint8_t *buf_end, const MIMEFieldWrapper& header, HEADER_INDEXING_TYPE type);

int64_t
decode_indexed_header_field(MIMEFieldWrapper& header, const uint8_t *buf_start, const uint8_t *buf_end, Http2HeaderTable& header_table);
int64_t
decode_literal_header_field(MIMEFieldWrapper& header, const uint8_t *buf_start, const uint8_t *buf_end, Http2HeaderTable& header_table);
int64_t
update_header_table_size(const uint8_t *buf_start, const uint8_t *buf_end, Http2HeaderTable& header_table);

#endif /* __HPACK_H__ */
