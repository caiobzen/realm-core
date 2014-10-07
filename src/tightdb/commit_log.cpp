/*************************************************************************
 *
 * TIGHTDB CONFIDENTIAL
 * __________________
 *
 *  [2011] - [2014] TightDB Inc
 *  All Rights Reserved.
 *
 * NOTICE:  All information contained herein is, and remains
 * the property of TightDB Incorporated and its suppliers,
 * if any.  The intellectual and technical concepts contained
 * herein are proprietary to TightDB Incorporated
 * and its suppliers and may be covered by U.S. and Foreign Patents,
 * patents in process, and are protected by trade secret or copyright law.
 * Dissemination of this information or reproduction of this material
 * is strictly forbidden unless prior written permission is obtained
 * from TightDB Incorporated.
 *
 **************************************************************************/

#include <exception>

#include <tightdb/util/unique_ptr.hpp>
#include <tightdb/util/thread.hpp>
#include <tightdb/group_shared.hpp>
#include <map>
#include <tightdb/commit_log.hpp>

#include <tightdb/replication.hpp>
#ifdef TIGHTDB_ENABLE_REPLICATION

namespace {

using namespace tightdb;

typedef uint_fast64_t version_type;

class WriteLogRegistry
{
    struct CommitEntry { std::size_t sz; char* data; };
public:
    WriteLogRegistry();
    ~WriteLogRegistry();

    void reset_log_management();
    void add_commit(version_type version, char* data, std::size_t sz);
    void get_commit_entries(version_type from, version_type to, BinaryData* commits) TIGHTDB_NOEXCEPT;
    void set_oldest_version_needed(version_type last_seen_version_number) TIGHTDB_NOEXCEPT;

private:
    // Get the index into the arrays for the selected version.
    size_t to_index(version_type version) { 
        TIGHTDB_ASSERT(version >= m_array_start);
        return version - m_array_start; 
    }
    version_type to_version(size_t idx)   { return idx + m_array_start; }
    bool holds_some_commits()             { return m_oldest_version != 0; }
    bool is_a_known_commit(version_type version);

    util::Mutex m_mutex;

    // array holding all commits. Array start with version 'm_array_start',
    std::vector<CommitEntry> m_commits;
    version_type m_array_start;

    // oldest and newest version stored - 0 in m_oldest_version indicates no versions stored
    version_type m_oldest_version;
    version_type m_newest_version;
};


bool WriteLogRegistry::is_a_known_commit(version_type version)
{
    return (holds_some_commits() && version >= m_oldest_version && version <= m_newest_version);
}


WriteLogRegistry::WriteLogRegistry()
{
    m_newest_version = 0;
    m_oldest_version = 0;
    // a version of 0 is never added, so having m_oldest_version==0 indicates
    // that no versions are present.
    m_array_start = 0;
}


WriteLogRegistry::~WriteLogRegistry()
{
    for (size_t i = 0; i < m_commits.size(); i++) {
        if (m_commits[i].data) {
            delete[] m_commits[i].data;
            m_commits[i].data = 0;
        }
    }
}


void WriteLogRegistry::reset_log_management()
{
    if (m_oldest_version) {
        // clear all old commits:
        for (version_type version = m_oldest_version; version <= m_newest_version; ++version) {
            size_t idx = to_index(version);
            if (m_commits[idx].data)
                delete[] m_commits[idx].data;
            m_commits[idx].data = 0;
            m_commits[idx].sz = 0;
        }
        m_newest_version = 0;
        m_oldest_version = 0;
    }
}


void WriteLogRegistry::add_commit(version_type version, char* data, std::size_t sz)
{
    util::LockGuard lock(m_mutex);

    if (!holds_some_commits()) {
        m_array_start = version;
        m_oldest_version = version;
    }
    else {
        TIGHTDB_ASSERT(version == 1 + m_newest_version);
    }
    CommitEntry ce = { sz, data };
    m_commits.push_back(ce);
    m_newest_version = version;
}



void WriteLogRegistry::get_commit_entries(version_type from, 
                                          version_type to, BinaryData* commits) TIGHTDB_NOEXCEPT
{
    util::LockGuard lock(m_mutex);
    size_t dest_idx = 0;
    for (version_type version = from+1; version <= to; version++) {
        TIGHTDB_ASSERT(is_a_known_commit(version));
        size_t idx = to_index(version);
        TIGHTDB_ASSERT(idx < m_commits.size());
        WriteLogRegistry::CommitEntry* entry = & m_commits[ idx ];
        commits[dest_idx] = BinaryData(entry->data, entry->sz);
        dest_idx++;
    }
}


void WriteLogRegistry::set_oldest_version_needed(version_type last_seen_version_number) TIGHTDB_NOEXCEPT
{
    util::LockGuard lock(m_mutex);

    // bail out early if no versions are stored
    if (! holds_some_commits()) return;

    version_type last_to_clean = last_seen_version_number;
    // do the actual cleanup, releasing commits in range [m_oldest_version .. last_to_clean]:
    for (version_type version = m_oldest_version; version <= last_to_clean; version++) {
        size_t idx = to_index(version);
        if (m_commits[idx].data)
            delete[] m_commits[idx].data;
        m_commits[idx].data = 0;
        m_commits[idx].sz = 0;
    }

    // realign or clear array of commits:
    if (last_to_clean == m_newest_version) {
        // special case: all commits have been released
        m_oldest_version = 0;
        m_array_start = 0;
        m_commits.resize(0);
    } 
    else {
        // some commits must be retained.
        m_oldest_version = last_to_clean + 1;

        if (to_index(m_oldest_version) > (m_commits.size() >> 1)) {
            // more than half of the commit array is free, so we'll
            // shift contents down and resize the array.

            size_t begin = to_index(m_oldest_version);
            size_t end = to_index(m_newest_version) + 1;

            std::copy(m_commits.begin() + begin,
                      m_commits.begin() + end,
                      m_commits.begin());
            m_commits.resize(m_newest_version - m_oldest_version + 1);
            m_array_start = m_oldest_version;
        }
    }
}







class RegistryRegistry {
public:
    WriteLogRegistry* get(std::string filepath);
    void add(std::string filepath, WriteLogRegistry* registry);
    void remove(std::string filepath);
    ~RegistryRegistry();
private:
    util::Mutex m_mutex;
    std::map<std::string, WriteLogRegistry*> m_registries;
};


WriteLogRegistry* RegistryRegistry::get(std::string filepath)
{
    util::LockGuard lock(m_mutex);
    std::map<std::string, WriteLogRegistry*>::iterator iter;
    iter = m_registries.find(filepath);
    if (iter != m_registries.end())
        return iter->second;
    WriteLogRegistry* result = new WriteLogRegistry;
    m_registries[filepath] = result;
    return result;
}

RegistryRegistry::~RegistryRegistry()
{
    std::map<std::string, WriteLogRegistry*>::iterator iter;
    iter = m_registries.begin();
    while (iter != m_registries.end()) {
        delete iter->second;
        iter->second = 0;
        ++iter;
    }
}

void RegistryRegistry::add(std::string filepath, WriteLogRegistry* registry)
{
    util::LockGuard lock(m_mutex);
    m_registries[filepath] = registry;
}


void RegistryRegistry::remove(std::string filepath)
{
    util::LockGuard lock(m_mutex);
    m_registries.erase(filepath);
}


RegistryRegistry globalRegistry;



} // anonymous namespace


namespace tightdb {


namespace _impl {

class WriteLogCollector : public Replication
{
public:
    WriteLogCollector(std::string database_name, WriteLogRegistry* registry);
    ~WriteLogCollector() TIGHTDB_NOEXCEPT;
    std::string do_get_database_path() TIGHTDB_OVERRIDE { return m_database_name; }
    void do_begin_write_transact(SharedGroup& sg) TIGHTDB_OVERRIDE;
    version_type do_commit_write_transact(SharedGroup& sg, version_type orig_version) TIGHTDB_OVERRIDE;
    void do_rollback_write_transact(SharedGroup& sg) TIGHTDB_NOEXCEPT TIGHTDB_OVERRIDE;
    void do_interrupt() TIGHTDB_NOEXCEPT TIGHTDB_OVERRIDE {};
    void do_clear_interrupt() TIGHTDB_NOEXCEPT TIGHTDB_OVERRIDE {};
    void do_transact_log_reserve(std::size_t sz) TIGHTDB_OVERRIDE;
    void do_transact_log_append(const char* data, std::size_t size) TIGHTDB_OVERRIDE;
    void transact_log_reserve(std::size_t n);
    virtual void reset_log_management() TIGHTDB_OVERRIDE;
    virtual void set_oldest_version_needed(uint_fast64_t last_seen_version_number) TIGHTDB_NOEXCEPT;
    virtual void get_commit_entries(uint_fast64_t from_version, uint_fast64_t to_version,
                                    BinaryData* logs_buffer) TIGHTDB_NOEXCEPT;
protected:
    std::string m_database_name;
    util::Buffer<char> m_transact_log_buffer;
    WriteLogRegistry* m_registry;
};



WriteLogCollector::~WriteLogCollector() TIGHTDB_NOEXCEPT 
{
}


void WriteLogCollector::set_oldest_version_needed(uint_fast64_t last_seen_version_number) TIGHTDB_NOEXCEPT
{
    m_registry->set_oldest_version_needed(last_seen_version_number);
}


void WriteLogCollector::get_commit_entries(uint_fast64_t from_version, uint_fast64_t to_version,
                                           BinaryData* logs_buffer) TIGHTDB_NOEXCEPT
{
    m_registry->get_commit_entries(from_version, to_version, logs_buffer);
}


void WriteLogCollector::reset_log_management()
{
    m_registry->reset_log_management();
}

void WriteLogCollector::do_begin_write_transact(SharedGroup& sg)
{
    static_cast<void>(sg);
    m_transact_log_free_begin = m_transact_log_buffer.data();
    m_transact_log_free_end   = m_transact_log_free_begin + m_transact_log_buffer.size();
}


WriteLogCollector::version_type 
WriteLogCollector::do_commit_write_transact(SharedGroup& sg, 
	WriteLogCollector::version_type orig_version)
{
    static_cast<void>(sg);
    char* data     = m_transact_log_buffer.release();
    std::size_t sz = m_transact_log_free_begin - data;
    version_type new_version = orig_version + 1;
    m_registry->add_commit(new_version, data, sz);
    return new_version;
}


void WriteLogCollector::do_rollback_write_transact(SharedGroup& sg) TIGHTDB_NOEXCEPT
{
    // forward transaction log buffer
    sg.do_rollback_and_continue_as_read(m_transact_log_buffer.data(), m_transact_log_free_begin);
}


void WriteLogCollector::do_transact_log_reserve(std::size_t sz)
{
    transact_log_reserve(sz);
}


void WriteLogCollector::do_transact_log_append(const char* data, std::size_t size)
{
    transact_log_reserve(size);
    m_transact_log_free_begin = std::copy(data, data+size, m_transact_log_free_begin);
}


void WriteLogCollector::transact_log_reserve(std::size_t n)
{
    char* data = m_transact_log_buffer.data();
    std::size_t size = m_transact_log_free_begin - data;
    m_transact_log_buffer.reserve_extra(size, n);
    data = m_transact_log_buffer.data();
    m_transact_log_free_begin = data + size;
    m_transact_log_free_end = data + m_transact_log_buffer.size();
}


WriteLogCollector::WriteLogCollector(std::string database_name, WriteLogRegistry* registry)
{
    m_database_name = database_name;
    m_registry = registry;
}

} // namespace _impl




Replication* makeWriteLogCollector(std::string database_name)
{
    WriteLogRegistry* registry = globalRegistry.get(database_name);
    return  new _impl::WriteLogCollector(database_name, registry);
}



} // namespace tightdb
#endif // TIGHTDB_ENABLE_REPLICATION
