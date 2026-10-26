/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include "common/json_type/ob_json_tree.h"
#include "common/json_type/ob_json_bin.h"
#include "lib/allocator/ob_allocator.h"

namespace oceanbase { namespace sql {

extern "C" {
void *seekdb_parquet_open_fd(int fd);
int seekdb_parquet_next(void *scan, const unsigned char **data, size_t *size);
void seekdb_parquet_close(void *scan);
}

// Native table-function input. At most one CSV record / JSON line is retained;
// schema discovery rewinds the input instead of materializing its rows.
class FileSqlReader
{
public:
  struct Field {
    std::string name;
    std::string text;
    common::ObObjType type = common::ObNullType;
    bool is_null = true;
  };
  struct Column {
    std::string name;
    common::ObObjType type = common::ObNullType;
    bool nullable = false;
  };
  FileSqlReader() : file_(nullptr), parquet_(nullptr), file_index_(0), header_(true), glob_index_(0) {}
  ~FileSqlReader() { close(); }
  FileSqlReader(const FileSqlReader &) = delete;
  FileSqlReader &operator=(const FileSqlReader &) = delete;

  static constexpr size_t MAX_RECORD_BYTES = 16 * 1024 * 1024;
  static constexpr size_t MAX_COLUMNS = 4096;

  // Option values may arrive as typed parameters, so accept any integer
  // representation instead of relying on one exact integer object type.
  static int const_to_int(const common::ObObj &obj, int64_t &value)
  {
    int ret = common::OB_SUCCESS;
    if (obj.is_tinyint() || obj.is_smallint() || obj.is_mediumint()
        || obj.is_int32() || obj.is_int()) {
      value = obj.get_int();
    } else {
      ret = common::OB_OBJ_TYPE_ERROR;
    }
    return ret;
  }

  int open(const std::string &path, const std::string &format, bool header,
           const std::string &secure_path)
  {
    close();
    files_.clear();
    format_ = format;
    header_ = header;
    secure_path_ = secure_path;
    int ret = common::OB_SUCCESS;
    if (path.empty() || path.find('\0') != std::string::npos
        || path.find("://") != std::string::npos || secure_path == "NULL") {
      ret = common::OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS;
    } else {
      glob_t matches = {};
      const int status = ::glob(path.c_str(), GLOB_NOSORT, nullptr, &matches);
      if (status != 0 && status != GLOB_NOMATCH) {
        ret = common::OB_IO_ERROR;
      } else {
        for (size_t i = 0; ret == common::OB_SUCCESS && i < matches.gl_pathc; ++i) {
          char resolved[PATH_MAX];
          struct stat info;
          if (::realpath(matches.gl_pathv[i], resolved) == nullptr) {
            ret = common::OB_FILE_NOT_EXIST;
          } else if (!is_allowed(resolved)) {
            ret = common::OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS;
          } else if (::stat(resolved, &info) != 0) {
            ret = common::OB_IO_ERROR;
          } else if (S_ISREG(info.st_mode)) {
            files_.push_back(resolved);
          }
        }
      }
      ::globfree(&matches);
      std::sort(files_.begin(), files_.end());
      if (ret == common::OB_SUCCESS && files_.empty() && format != "glob") {
        ret = common::OB_FILE_NOT_EXIST;
      }
    }
    file_index_ = 0;
    glob_index_ = 0;
    return ret;
  }

  int infer(int64_t sample_size, std::vector<Column> &columns)
  {
    using namespace common;
    columns.clear();
    int ret = OB_SUCCESS;
    if (format_ == "glob") {
      columns.push_back({"file", ObVarcharType, false});
    } else {
      int64_t rows = 0;
      bool at_end = false;
      std::vector<Field> fields;
      while (ret == OB_SUCCESS && (sample_size < 0 || rows < sample_size)) {
        ret = next(fields);
        if (ret == OB_ITER_END) { at_end = true; ret = OB_SUCCESS; break; }
        if (ret != OB_SUCCESS) { break; }
        for (const auto &f : fields) {
          auto it = std::find_if(columns.begin(), columns.end(), [&](const Column &c) {
            return equal_name(c.name, f.name);
          });
          if (it == columns.end()) {
            if (columns.size() >= MAX_COLUMNS) { ret = OB_SIZE_OVERFLOW; break; }
            columns.push_back({f.name, f.type, rows > 0 || f.is_null});
          } else {
            it->nullable |= f.is_null;
            it->type = merge_type(it->type, f.type);
          }
        }
        for (auto &c : columns) {
          c.nullable |= std::none_of(fields.begin(), fields.end(), [&](const Field &f) {
            return equal_name(c.name, f.name);
          });
        }
        ++rows;
      }
      // A header-only CSV still defines a useful, nullable VARCHAR schema.
      if (ret == OB_SUCCESS && columns.empty()) {
        for (const auto &name : names_) { columns.push_back({name, ObVarcharType, true}); }
      }
      for (auto &c : columns) {
        c.nullable |= !at_end;
        if (c.type == ObNullType) { c.type = ObVarcharType; c.nullable = true; }
      }
      if (ret == OB_SUCCESS && columns.empty()) { ret = OB_INVALID_ARGUMENT; }
    }
    close();
    file_index_ = 0;
    glob_index_ = 0;
    names_.clear();
    return ret;
  }

  int next(std::vector<Field> &fields)
  {
    using namespace common;
    fields.clear();
    if (format_ == "glob") {
      if (glob_index_ >= files_.size()) { return OB_ITER_END; }
      fields.push_back({"file", files_[glob_index_++], ObVarcharType, false});
      return OB_SUCCESS;
    }
    int ret = OB_SUCCESS;
    for (;;) {
      if (file_ == nullptr && parquet_ == nullptr) {
        if (file_index_ >= files_.size()) { return OB_ITER_END; }
        if ((ret = open_next_file()) != OB_SUCCESS) { return ret; }
      }
      if (format_ == "csv") {
        if ((ret = csv_record(fields)) == OB_SUCCESS) {
          if (names_.empty()) {
            if (header_) {
              for (size_t i = 0; i < fields.size(); ++i) {
                std::string name = fields[i].text;
                if (i == 0 && name.compare(0, 3, "\xEF\xBB\xBF") == 0) { name.erase(0, 3); }
                if (name.empty()) { name = "column" + std::to_string(i); }
                std::string unique = name;
                size_t suffix = 2;
                while (std::any_of(names_.begin(), names_.end(), [&](const std::string &n) {
                  return equal_name(n, unique);
                })) { unique = name + "__" + std::to_string(suffix++); }
                names_.push_back(unique);
              }
              fields.clear();
              continue;
            } else {
              for (size_t i = 0; i < fields.size(); ++i) { names_.push_back("column" + std::to_string(i)); }
            }
          }
          if (fields.size() != names_.size()) { return OB_INVALID_ARGUMENT; }
          for (size_t i = 0; i < fields.size(); ++i) { fields[i].name = names_[i]; }
        }
      } else if (format_ == "json") {
        ret = json_record(fields);
      } else if (format_ == "parquet") {
        const unsigned char *data = nullptr;
        size_t size = 0;
        const int result = seekdb_parquet_next(parquet_, &data, &size);
        if (result == 1) { ret = OB_ITER_END; }
        else if (result != 0) { ret = OB_INVALID_ARGUMENT; }
        else if (size > MAX_RECORD_BYTES) { ret = OB_SIZE_OVERFLOW; }
        else { ret = parse_json(std::string(reinterpret_cast<const char*>(data), size), fields); }
      } else {
        return OB_NOT_SUPPORTED;
      }
      if (ret == OB_ITER_END) { close(); names_.clear(); continue; }
      return ret;
    }
  }

  void close()
  {
    if (file_ != nullptr) { ::fclose(file_); file_ = nullptr; }
    if (parquet_ != nullptr) { seekdb_parquet_close(parquet_); parquet_ = nullptr; }
  }
  static bool equal_name(const std::string &a, const std::string &b)
  {
    return a.size() == b.size() && ::strncasecmp(a.data(), b.data(), a.size()) == 0;
  }

private:
  bool is_allowed(const char *path) const
  {
    if (secure_path_.empty() || equal_name(secure_path_, "NULL")) { return false; }
    if (secure_path_ == "/") { return true; }
    char allowed[PATH_MAX];
    if (::realpath(secure_path_.c_str(), allowed) == nullptr) { return false; }
    const size_t len = ::strlen(allowed);
    return ::strncmp(path, allowed, len) == 0 && (path[len] == '/' || path[len] == '\0');
  }
  int open_next_file()
  {
    using namespace common;
    const std::string &path = files_[file_index_++];
    struct stat before, after;
    if (!is_allowed(path.c_str())) { return OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS; }
    if (::stat(path.c_str(), &before) != 0) { return OB_FILE_NOT_EXIST; }
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) { return OB_IO_ERROR; }
    if (::fstat(fd, &after) != 0 || !S_ISREG(after.st_mode)
        || before.st_dev != after.st_dev || before.st_ino != after.st_ino) {
      ::close(fd);
      return OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS;
    }
    if (format_ == "parquet") {
      parquet_ = seekdb_parquet_open_fd(fd);
      return parquet_ == nullptr ? OB_INVALID_ARGUMENT : OB_SUCCESS;
    }
    file_ = ::fdopen(fd, "rb");
    if (file_ == nullptr) { ::close(fd); return OB_IO_ERROR; }
    return OB_SUCCESS;
  }

  static common::ObObjType classify(const std::string &s)
  {
    using namespace common;
    if (equal_name(s, "true") || equal_name(s, "false")) { return ObTinyIntType; }
    if (s.empty()) { return ObVarcharType; }
    char *end = nullptr;
    errno = 0;
    ::strtoll(s.c_str(), &end, 10);
    if (end != s.c_str() && *end == '\0' && errno != ERANGE) { return ObIntType; }
    errno = 0;
    const double d = ::strtod(s.c_str(), &end);
    if (end != s.c_str() && *end == '\0' && errno != ERANGE && std::isfinite(d)) { return ObDoubleType; }
    if (s.size() == 10 || s.size() == 19) {
      bool date = true;
      for (size_t i = 0; i < s.size(); ++i) {
        const char expected = (i == 4 || i == 7) ? '-' : i == 10 ? ' ' : (i == 13 || i == 16) ? ':' : '\0';
        date &= i == 10 ? (s[i] == ' ' || s[i] == 'T') : expected ? s[i] == expected : s[i] >= '0' && s[i] <= '9';
      }
      if (date) { return s.size() == 10 ? ObDateType : ObDateTimeType; }
    }
    return ObVarcharType;
  }
  static common::ObObjType merge_type(common::ObObjType a, common::ObObjType b)
  {
    using namespace common;
    if (a == ObNullType) { return b; }
    if (b == ObNullType || a == b) { return a; }
    auto numeric = [](ObObjType t) { return t == ObTinyIntType || t == ObIntType || t == ObDoubleType; };
    if (numeric(a) && numeric(b)) {
      return a == ObDoubleType || b == ObDoubleType ? ObDoubleType : ObIntType;
    }
    return ObVarcharType;
  }

  int csv_record(std::vector<Field> &fields)
  {
    using namespace common;
    std::string value;
    bool quoted = false, in_quotes = false, ended_quote = false, started = false;
    size_t bytes = 0;
    for (;;) {
      const int ch = ::fgetc(file_);
      if (ch == EOF && ::ferror(file_)) { return OB_IO_ERROR; }
      if (ch != EOF && ++bytes > MAX_RECORD_BYTES) { return OB_SIZE_OVERFLOW; }
      if ((ch == '\n' || ch == '\r') && !started && fields.empty() && value.empty()) {
        if (ch == '\r') { const int next = ::fgetc(file_); if (next != '\n' && next != EOF) { ::ungetc(next, file_); } }
        continue;
      }
      if (ch == EOF && !started && fields.empty() && value.empty()) { return OB_ITER_END; }
      if (ch == EOF && in_quotes) { return OB_INVALID_ARGUMENT; }
      if (in_quotes) {
        if (ch == '"') {
          const int next = ::fgetc(file_);
          if (next == '"') { value += '"'; ++bytes; }
          else { in_quotes = false; ended_quote = true; if (next != EOF) { ::ungetc(next, file_); } }
        } else { value += static_cast<char>(ch); }
      } else if (ch == ',' || ch == '\n' || ch == '\r' || ch == EOF) {
        const bool is_null = !quoted && (value.empty() || equal_name(value, "NULL"));
        fields.push_back({"", value, is_null ? ObNullType : classify(value), is_null});
        if (fields.size() > MAX_COLUMNS) { return OB_SIZE_OVERFLOW; }
        if (ch != ',') {
          if (ch == '\r') { const int next = ::fgetc(file_); if (next != '\n' && next != EOF) { ::ungetc(next, file_); } }
          return OB_SUCCESS;
        }
        value.clear(); quoted = false; ended_quote = false; started = false;
        continue;
      } else if (ch == '"' && !started) {
        quoted = true; in_quotes = true;
      } else if (ended_quote || ch == '"') {
        return OB_INVALID_ARGUMENT;
      } else { value += static_cast<char>(ch); }
      started = true;
    }
  }

  int json_record(std::vector<Field> &fields)
  {
    using namespace common;
    std::string line;
    for (;;) {
      const int ch = ::fgetc(file_);
      if (ch == EOF && ::ferror(file_)) { return OB_IO_ERROR; }
      if (ch == EOF && line.empty()) { return OB_ITER_END; }
      if (ch == '\n' || ch == EOF) {
        if (line.find_first_not_of(" \t\r") == std::string::npos) { line.clear(); continue; }
        break;
      }
      line += static_cast<char>(ch);
      if (line.size() > MAX_RECORD_BYTES) { return OB_SIZE_OVERFLOW; }
    }
    return parse_json(line, fields);
  }

  int parse_json(const std::string &line, std::vector<Field> &fields)
  {
    using namespace common;
    ObArenaAllocator alloc;
    ObIJsonBase *json = nullptr;
    int ret = ObJsonBaseFactory::get_json_base(&alloc, ObString(line.size(), line.data()),
                                               ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, json);
    if (ret != OB_SUCCESS) { return ret; }
    if (json == nullptr || json->json_type() != ObJsonNodeType::J_OBJECT) { return OB_INVALID_ARGUMENT; }
    if (json->element_count() > MAX_COLUMNS) { return OB_SIZE_OVERFLOW; }
    for (uint64_t i = 0; ret == OB_SUCCESS && i < json->element_count(); ++i) {
      ObString key;
      ObIJsonBase *val = nullptr;
      ret = json->get_key(i, key);
      if (ret == OB_SUCCESS) { ret = json->get_object_value(i, val); }
      if (ret != OB_SUCCESS || val == nullptr) { return ret == OB_SUCCESS ? OB_ERR_UNEXPECTED : ret; }
      Field f;
      f.name.assign(key.ptr(), key.length());
      f.is_null = val->json_type() == ObJsonNodeType::J_NULL;
      if (!f.is_null) {
        if (val->json_type() == ObJsonNodeType::J_STRING) {
          f.text.assign(val->get_data(), val->get_data_length()); f.type = ObVarcharType;
        } else {
          ObJsonBuffer output(&alloc);
          if ((ret = val->print(output, false)) != OB_SUCCESS) { return ret; }
          f.text.assign(output.ptr(), output.length());
          f.type = classify(f.text);
        }
      }
      fields.push_back(std::move(f));
    }
    return ret;
  }

  FILE *file_;
  void *parquet_;
  size_t file_index_;
  bool header_;
  size_t glob_index_;
  std::string format_, secure_path_;
  std::vector<std::string> files_, names_;
};

} }
