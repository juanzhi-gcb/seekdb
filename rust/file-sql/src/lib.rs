// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
// This module decodes Parquet records only. SQL planning and evaluation stay in
// seekdb. The iterator loads row groups on demand and never retains a whole file.

use parquet::file::serialized_reader::SerializedFileReader;
use parquet::record::reader::RowIter;
use std::fs::File;
use std::os::fd::{FromRawFd, RawFd};
use std::ptr;

pub struct ParquetScan {
    rows: RowIter<'static>,
    current: Vec<u8>,
}

/// Takes ownership of an already securely opened descriptor, including on error.
/// The C++ caller checks secure_file_priv and the file's identity before calling.
#[no_mangle]
pub unsafe extern "C" fn seekdb_parquet_open_fd(fd: RawFd) -> *mut ParquetScan {
    if fd < 0 {
        return ptr::null_mut();
    }
    let file = File::from_raw_fd(fd);
    match SerializedFileReader::new(file) {
        Ok(reader) => Box::into_raw(Box::new(ParquetScan {
            rows: reader.into_iter(),
            current: Vec::new(),
        })),
        Err(_) => ptr::null_mut(),
    }
}

/// Returns 0 for a row, 1 for EOF, -1 for an invalid stream. Returned bytes are
/// owned by the scan and remain valid until its next call or destruction.
#[no_mangle]
pub unsafe extern "C" fn seekdb_parquet_next(
    scan: *mut ParquetScan,
    data: *mut *const u8,
    size: *mut usize,
) -> i32 {
    if scan.is_null() || data.is_null() || size.is_null() {
        return -1;
    }
    *data = ptr::null();
    *size = 0;
    let scan = &mut *scan;
    match scan.rows.next() {
        None => 1,
        Some(Err(_)) => -1,
        Some(Ok(row)) => match serde_json::to_vec(&row.to_json_value()) {
            Err(_) => -1,
            Ok(bytes) => {
                scan.current = bytes;
                *data = scan.current.as_ptr();
                *size = scan.current.len();
                0
            }
        },
    }
}

#[no_mangle]
pub unsafe extern "C" fn seekdb_parquet_close(scan: *mut ParquetScan) {
    if !scan.is_null() {
        drop(Box::from_raw(scan));
    }
}
