#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/program_options.hpp>
#include "librados/librados_asio.h"
#include "global/global_init.h"
#include "global/global_context.h"

#define dout_context g_ceph_context

namespace ceph {
  namespace consistency {
    typedef std::pair<std::string, ceph::bufferlist> ReadResult;
    class Read {
      protected:
        std::string oid;
        uint64_t block_size;
        uint64_t offset;
        uint64_t length;
      
      public:
        Read(const std::string& oid,
             uint64_t block_size,
             uint64_t offset,
             uint64_t length);
        std::string get_oid(void);
        uint64_t get_block_size(void);
        uint64_t get_offset(void);
        uint64_t get_length(void);
    };
    class ECReader {
      protected:
        librados::Rados& rados;
        boost::asio::io_context& asio;
        std::string pool_name;
        std::string oid;
        librados::IoCtx io;
        librados::ObjectReadOperation op;
        ceph::condition_variable cond;
        std::vector<ReadResult> results;
        ceph::mutex lock;
        int outstanding_io;

      public:
        ECReader(librados::Rados& rados, boost::asio::io_context& asio, const std::string& pool);
        void do_read(Read read);
        void start_io(void);
        void finish_io(void);
        void wait_for_io(void);
        std::vector<ReadResult>* get_results(void);
    };
  }
}