/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include "sql/engine/basic/file_sql_reader.h"

namespace oceanbase { namespace sql {

class FileSqlWriter
{
public:
  FileSqlWriter() : file_(nullptr) {}
  ~FileSqlWriter() { close(); }
  int open(const std::string &path, const std::string &secure, const common::ObString &options)
  {
    using namespace common;
    if (path.empty() || path.find('\0') != std::string::npos || path.find("://") != std::string::npos) {
      return OB_INVALID_ARGUMENT;
    }
    const size_t slash = path.find_last_of('/');
    const std::string parent = slash == std::string::npos ? "." : slash == 0 ? "/" : path.substr(0, slash);
    const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    if (name.empty() || name == "." || name == "..") { return OB_INVALID_ARGUMENT; }
    char real_parent[PATH_MAX], real_secure[PATH_MAX];
    if (::realpath(parent.c_str(), real_parent) == nullptr) { return OB_FILE_NOT_EXIST; }
    if (secure.empty() || FileSqlReader::equal_name(secure, "NULL")
        || ::realpath(secure.c_str(), real_secure) == nullptr) { return OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS; }
    const std::string base(real_parent), allowed(real_secure);
    if (allowed != "/" && base != allowed && base.compare(0, allowed.size() + 1, allowed + "/") != 0) {
      return OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS;
    }
    ObArenaAllocator alloc;
    ObIJsonBase *opts = nullptr, *headers = nullptr, *nullstr = nullptr;
    int ret = ObJsonBaseFactory::get_json_base(&alloc, options, ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, opts);
    if (ret != OB_SUCCESS) { return ret; }
    if (opts == nullptr || opts->json_type() != ObJsonNodeType::J_OBJECT) { return OB_INVALID_ARGUMENT; }
    if ((ret = opts->get_object_value("nullstr", nullstr)) != OB_SUCCESS
        || nullstr->json_type() != ObJsonNodeType::J_STRING) { return OB_INVALID_ARGUMENT; }
    nullstr_.assign(nullstr->get_data(), nullstr->get_data_length());
    if ((ret = opts->get_object_value("headers", headers)) != OB_SUCCESS
        || headers->json_type() != ObJsonNodeType::J_ARRAY) { return OB_INVALID_ARGUMENT; }
    struct stat before, after;
    if (::stat(real_parent, &before) != 0) { return OB_IO_ERROR; }
    const int dir = ::open(real_parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dir < 0) { return OB_IO_ERROR; }
    if (::fstat(dir, &after) != 0 || before.st_dev != after.st_dev || before.st_ino != after.st_ino) {
      ::close(dir); return OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS;
    }
    const int fd = ::openat(dir, name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    ::close(dir);
    if (fd < 0) { return OB_IO_ERROR; }
    file_ = ::fdopen(fd, "wb");
    if (file_ == nullptr) { ::close(fd); return OB_IO_ERROR; }
    for (uint64_t i = 0; ret == OB_SUCCESS && i < headers->element_count(); ++i) {
      ObIJsonBase *value = nullptr;
      ret = headers->get_array_element(i, value);
      if (ret == OB_SUCCESS && (value == nullptr || value->json_type() != ObJsonNodeType::J_STRING)) {
        ret = OB_INVALID_ARGUMENT;
      }
      if (ret == OB_SUCCESS) { ret = field(ObString(value->get_data_length(), value->get_data()), false, i == 0); }
    }
    if (ret == OB_SUCCESS && headers->element_count() > 0) { ret = end_row(); }
    return ret;
  }

  int field(const common::ObString &text, bool is_null, bool first)
  {
    using namespace common;
    if (file_ == nullptr) { return OB_NOT_INIT; }
    if (!first && ::fputc(',', file_) == EOF) { return OB_IO_ERROR; }
    if (is_null) {
      return ::fwrite(nullstr_.data(), 1, nullstr_.size(), file_) == nullstr_.size() ? OB_SUCCESS : OB_IO_ERROR;
    }
    if (::fputc('"', file_) == EOF) { return OB_IO_ERROR; }
    for (int64_t i = 0; i < text.length(); ++i) {
      if (::fputc(text.ptr()[i], file_) == EOF || (text.ptr()[i] == '"' && ::fputc('"', file_) == EOF)) {
        return OB_IO_ERROR;
      }
    }
    return ::fputc('"', file_) == EOF ? OB_IO_ERROR : OB_SUCCESS;
  }
  int end_row() { return ::fputc('\n', file_) == EOF ? common::OB_IO_ERROR : common::OB_SUCCESS; }
  int close()
  {
    int ret = common::OB_SUCCESS;
    if (file_ != nullptr) {
      if (::fclose(file_) != 0) { ret = common::OB_IO_ERROR; }
      file_ = nullptr;
    }
    return ret;
  }
private:
  FILE *file_;
  std::string nullstr_;
};
} }
