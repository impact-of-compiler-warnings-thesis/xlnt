// Copyright (C) 2016-2017 Thomas Fussell
// Copyright (C) 2002-2007 Ariya Hidayat (ariya@kde.org).
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//
// 1. Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
// IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
// OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
// IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
// INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
// NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
// THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 
#include <array>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <list>
#include <string>
#include <vector>

#include <detail/binary.hpp>
#include <detail/cryptography/compound_document.hpp>
#include <xlnt/utils/exceptions.hpp>

namespace {

using xlnt::detail::byte;
using xlnt::detail::binary_reader;
using xlnt::detail::binary_writer;

using directory_id = std::int32_t;
using sector_id = std::int32_t;

class allocation_table
{
public:
    static const sector_id FreeSector;
    static const sector_id EndOfChainSector;
    static const sector_id AllocationTableSector;
    static const sector_id MasterAllocationTableSector;

    allocation_table()
    {
        resize(128);
    }

    std::size_t count() const
    {
        return data_.size();
    }

    void resize(std::size_t newsize)
    {
        data_.resize(newsize, FreeSector);
    }
/*
    void set(sector_id index, sector_id value)
    {
        if (static_cast<std::size_t>(index) >= count())
        {
            resize(static_cast<std::size_t>(index + 1));
        }

        data_[static_cast<std::size_t>(index)] = value;
    }
*/
/*
    void setChain(std::vector<sector_id> chain)
    {
        if (chain.size())
        {
            for (std::size_t i = 0; i < chain.size() - 1; i++)
            {
                set(chain[i], chain[i + 1]);
            }

            set(chain[chain.size() - 1], EndOfChainSector);
        }
    }
*/
    std::vector<sector_id> follow(sector_id start) const
    {
        auto chain = std::vector<sector_id>();

        if (start >= static_cast<sector_id>(count()))
        {
            return chain;
        }

        auto p = start;

        auto already_exists = [](const std::vector<sector_id> &check, sector_id item)
        {
            for (std::size_t i = 0; i < check.size(); i++)
            {
                if (check[i] == item) return true;
            }

            return false;
        };

        while (p < static_cast<sector_id>(count()))
        {
            if (p == EndOfChainSector) break;
            if (p == AllocationTableSector) break;
            if (p == MasterAllocationTableSector) break;
            if (already_exists(chain, p)) break;
            chain.push_back(p);
            if (data_[static_cast<std::size_t>(p)] >= static_cast<sector_id>(count())) break;
            p = data_[static_cast<std::size_t>(p)];
        }

        return chain;
    }

    void load(const std::vector<byte> &sectors)
    {
        auto reader = binary_reader(sectors);
        data_ = reader.as_vector_of<sector_id>();
    }

    std::size_t sector_size() const
    {
        return sector_size_;
    }

    void sector_size(std::size_t size)
    {
        sector_size_ = size;
    }

private:
    std::size_t sector_size_ = 4096;
    std::vector<sector_id> data_;
};

const sector_id allocation_table::FreeSector = -1;
const sector_id allocation_table::EndOfChainSector = -2;
const sector_id allocation_table::AllocationTableSector = -3;
const sector_id allocation_table::MasterAllocationTableSector = -4;

class header
{
public:
    header()
    {
    }

    bool is_valid() const
    {
        if (threshold_ != 4096) return false;
        if (num_sectors_ == 0) return false;
        if ((num_sectors_ > 109) && (num_sectors_ > (num_master_sectors_ * 127) + 109)) return false;
        if ((num_sectors_ < 109) && (num_master_sectors_ != 0)) return false;
        if (short_sector_size_power_ > sector_size_power_) return false;
        if (sector_size_power_ <= 6) return false;
        if (sector_size_power_ >= 31) return false;

        return true;
    }

    void load(binary_reader &reader)
    {
        if (reader.size() < 512)
        {
            throw xlnt::exception("bad header");
        }

        *this = reader.read<header>();

        if (file_id_ != 0xe11ab1a1e011cfd0)
        {
            throw xlnt::exception("not ole");
        }

        if (!is_valid())
        {
            throw xlnt::exception("bad ole");
        }
    }

    std::size_t sector_size() const
    {
        return std::size_t(1) << sector_size_power_;
    }

    std::size_t short_sector_size() const
    {
        return std::size_t(1) << short_sector_size_power_;
    }

    std::vector<sector_id> sectors() const
    {
        const auto num_header_sectors = std::min(num_sectors_, std::uint32_t(109));
        return std::vector<sector_id>(
            first_master_table.begin(), 
            first_master_table.begin() + num_header_sectors);
    }

    std::size_t num_master_sectors() const
    {
        return static_cast<std::size_t>(num_master_sectors_);
    }

    sector_id master_table_start() const
    {
        return master_start_;
    }

    sector_id short_table_start() const
    {
        return short_start_;
    }

    sector_id directory_start() const
    {
        return directory_start_;
    }

    std::size_t threshold() const
    {
        return threshold_;
    }

private:
    std::uint64_t file_id_ = 0xe11ab1a1e011cfd0;
    std::array<std::uint8_t, 16> ignore1 = {{0}};
    std::uint16_t revision_ = 0x003E;
    std::uint16_t version_ = 0x0003;
    std::uint16_t byte_order_ = 0xFEFF;
    std::uint16_t sector_size_power_ = 9;
    std::uint16_t short_sector_size_power_ = 6;
    std::array<std::uint8_t, 10> ignore2 = {{0}};
    std::uint32_t num_sectors_ = 0;
    sector_id directory_start_ = 0;
    std::array<std::uint8_t, 4> ignore3 = {{0}};
    std::uint32_t threshold_ = 4096;
    sector_id short_start_ = 0;
    std::uint32_t num_short_sectors_ = 0;
    sector_id master_start_ = 0;
    std::uint32_t num_master_sectors_ = 0;
    std::array<sector_id, 109> first_master_table = {{allocation_table::FreeSector}};
};

struct directory_entry
{
    std::array<char16_t, 32> name = {{0}};
    std::uint16_t name_length = 0;

    enum class entry_type : std::uint8_t
    {
        Empty = 0,
        UserStorage = 1,
        UserStream = 2,
        LockBytes = 3,
        Property = 4,
        RootStorage = 5
    } type;

    enum class entry_color : std::uint8_t
    {
        Red = 0,
        Black = 1
    } color;

    directory_id prev = -1;
    directory_id next = -1;
    directory_id child = -1;

    std::array<std::uint8_t, 36> ignore;

    sector_id first = 0;

    std::uint32_t size = 0;

    std::uint32_t ignore2;
};

class directory_tree
{
public:
    //static const directory_id End = -1;

    static void entry_name(directory_entry &entry, std::u16string name)
    {
        if (name.size() > 31)
        {
            name.resize(31);
        }

        std::copy(name.begin(), name.end(), entry.name.begin());
        entry.name[name.size()] = 0;
        entry.name_length = static_cast<std::uint16_t>((name.size() + 1) * 2);
    }

    directory_tree()
        : entries()
    {
        clear();
    }

    void clear()
    {
        entries = { create_root_entry() };
    }

    std::size_t entry_count() const
    {
        return entries.size();
    }

    directory_entry &entry(directory_id index)
    {
        return entries[static_cast<std::size_t>(index)];
    }

    const directory_entry &entry(directory_id index) const
    {
        return entries[static_cast<std::size_t>(index)];
    }

    const directory_entry &entry(const std::u16string &name) const
    {
        return entry(find_entry(name).first);
    }

    directory_entry &entry(const std::u16string &name, bool create)
    {
        auto find_result = find_entry(name);
        auto index = find_result.first;
        auto found = find_result.second;

        if (!found)
        {
            // not found among children
            if (!create)
            {
                throw xlnt::exception("not found");
            }

            // create a new entry
            auto parent = index;
            entries.push_back(directory_entry());
            index = static_cast<directory_id>(entry_count() - 1);
            auto &e = entry(index);
            e.first = 0;
            entry(parent).prev = index;
        }

        return entry(index);
    }
/*
    directory_id parent(directory_id index)
    {
        // brute-force, basically we iterate for each entries, find its children
        // and check if one of the children is 'index'
        for (auto j = directory_id(0); j < static_cast<directory_id>(entry_count()); j++)
        {
            auto chi = children(j);

            for (std::size_t i = 0; i < chi.size(); i++)
            {
                if (chi[i] == index)
                {
                    return j;
                }
            }
        }

        return -1;
    }
*/
/*
    std::u16string path(directory_id index)
    {
        // don't use root name ("Root Entry"), just give "/"
        if (index == 0) return u"/";

        auto current_entry = entry(index);

        auto result = std::u16string(entry(index).name.data());
        result.insert(0, u"/");

        auto current_parent = parent(index);

        while (current_parent > 0)
        {
            current_entry = entry(current_parent);

            result.insert(0, std::u16string(current_entry.name.data()));
            result.insert(0, u"/");

            --current_parent;
            index = current_parent;

            if (current_parent <= 0) break;
        }

        return result;
    }
*/
    std::vector<directory_id> children(directory_id index) const
    {
        auto result = std::vector<directory_id>();
        auto &e = entry(index);

        if (e.child >= 0 && e.child < static_cast<directory_id>(entry_count()))
        {
            find_siblings(result, e.child);
        }

        return result;
    }


    void load(const std::vector<byte> &data)
    {
        auto reader = binary_reader(data);
        entries = reader.as_vector_of<directory_entry>();
        
        auto is_empty = [](const directory_entry &entry)
        {
            return entry.type == directory_entry::entry_type::Empty;
        };

        entries.erase(std::remove_if(entries.begin(), entries.end(), is_empty));
    }

    directory_entry create_root_entry() const
    {
        directory_entry root;

        entry_name(root, u"Root Entry");
        root.type = directory_entry::entry_type::RootStorage;
        root.color = directory_entry::entry_color::Black;
        root.size = 0;

        return root;
    }

private:
    // helper function: recursively find siblings of index
    void find_siblings(std::vector<directory_id> &result, directory_id index) const
    {
        auto e = entry(index);

        // prevent infinite loop
        for (std::size_t i = 0; i < result.size(); i++)
        {
            if (result[i] == index) return;
        }

        // add myself
        result.push_back(index);

        // visit previous sibling, don't go infinitely
        auto prev = e.prev;

        if ((prev > 0) && (prev < static_cast<directory_id>(entry_count())))
        {
            for (std::size_t i = 0; i < result.size(); i++)
            {
                if (result[i] == prev)
                {
                    prev = 0;
                }
            }

            if (prev)
            {
                find_siblings(result, prev);
            }
        }

        // visit next sibling, don't go infinitely
        auto next = e.next;

        if ((next > 0) && (next < static_cast<directory_id>(entry_count())))
        {
            for (std::size_t i = 0; i < result.size(); i++)
            {
                if (result[i] == next) next = 0;
            }

            if (next)
            {
                find_siblings(result, next);
            }
        }
    }

    std::pair<directory_id, bool> find_entry(const std::u16string &name) const
    {
        // quick check for "/" (that's root)
        if (name == u"/Root Entry")
        {
            return { 0, true };
        }

        // split the names, e.g  "/ObjectPool/_1020961869" will become:
        // "ObjectPool" and "_1020961869"
        auto names = std::vector<std::u16string>();
        auto start = std::size_t(0);
        auto end = std::size_t(0);

        if (name[0] == u'/') start++;

        while (start < name.length())
        {
            end = name.find_first_of('/', start);
            if (end == std::string::npos) end = name.length();
            names.push_back(name.substr(start, end - start));
            start = end + 1;
        }

        // start from root
        auto index = directory_id(0);

        for (auto it = names.begin(); it != names.end(); ++it)
        {
            // find among the children of index
            auto chi = children(index);
            std::ptrdiff_t child = 0;

            for (std::size_t i = 0; i < chi.size(); i++)
            {
                auto ce = entry(chi[i]);

                if (std::u16string(ce.name.data()) == *it)
                {
                    child = static_cast<std::ptrdiff_t>(chi[i]);
                }
            }

            // traverse to the child
            if (child > 0)
            {
                index = static_cast<directory_id>(child);
            }
            else
            {
                return { index, false };
            }
        }

        return { index, true };
    }

    std::vector<directory_entry> entries;
};

} // namespace

namespace xlnt {
namespace detail {

class compound_document_reader_impl
{
public:
    compound_document_reader_impl(const std::vector<byte> &bytes)
        : sectors_(bytes.data() + sizeof(header)),
          sectors_size_(bytes.size())
    {
        auto reader = binary_reader(bytes);

        header_.load(reader);

        const auto sector_size = header_.sector_size();
        sector_table_.sector_size(sector_size);
        const auto master_table_chain = load_master_table();
        const auto master_sectors = read(master_table_chain);
        sector_table_.load(master_sectors);

        const auto short_sector_size = header_.short_sector_size();
        short_sector_table_.sector_size(short_sector_size);
        const auto short_start = header_.short_table_start();
        const auto short_table_chain = sector_table_.follow(short_start);
        const auto short_sectors = read(short_table_chain);
        short_sector_table_.load(short_sectors);

        const auto directory_start = header_.directory_start();
        const auto directory_chain = sector_table_.follow(directory_start);
        const auto directory_sectors = read(directory_chain);
        directory_.load(directory_sectors);

        auto first_short_sector = directory_.entry(u"/Root Entry", false).first;
        short_container_stream_ = sector_table_.follow(first_short_sector);
    }

    std::vector<byte> read(const std::vector<sector_id> &sectors) const
    {
        const auto sector_size = sector_table_.sector_size();
        auto result = std::vector<byte>();
        auto writer = binary_writer(result);

        for (auto sector : sectors)
        {
            auto position = sector_size * static_cast<std::size_t>(sector);
            writer.append(sectors_, sectors_size_, position, sector_size);
        }
        
        return result;
    }

    std::vector<byte> read_short(const std::vector<sector_id> &sectors) const
    {
        const auto short_sector_size = short_sector_table_.sector_size();
        const auto sector_size = sector_table_.sector_size();
        auto result = std::vector<byte>();
        auto writer = binary_writer(result);

        for (auto sector : sectors)
        {
            auto position = short_sector_size * static_cast<std::size_t>(sector);
            auto master_allocation_table_index = position / sector_size;

            auto sector_data = read({ short_container_stream_[master_allocation_table_index] });

            auto offset = position % sector_size;
            writer.append(sector_data, offset, short_sector_size);
        }

        return result;
    }

    std::vector<sector_id> load_master_table()
    {
        auto master_sectors = header_.sectors();

        if (header_.num_master_sectors() > 109)
        {
            auto current_sector = header_.master_table_start();

            for (auto r = std::size_t(0); r < header_.num_master_sectors(); ++r)
            {
                auto current_sector_data = read({ current_sector });
                auto current_sector_reader = binary_reader(current_sector_data);
                auto current_sector_sectors = current_sector_reader.as_vector_of<sector_id>();
                
                current_sector = current_sector_sectors.back();
                current_sector_sectors.pop_back();
                
                master_sectors.insert(
                    current_sector_sectors.begin(),
                    current_sector_sectors.end(),
                    master_sectors.end());
            }
        }

        return master_sectors;
    }

    std::vector<byte> read_stream(const std::u16string &name) const
    {
        const auto entry = directory_.entry(name);

        auto result = entry.size < header_.threshold()
            ? read_short(short_sector_table_.follow(entry.first))
            : read(sector_table_.follow(entry.first));
        result.resize(entry.size);

        return result;
    }

private:
    const byte *sectors_;
    const std::size_t sectors_size_;
    directory_tree directory_;
    header header_;
    allocation_table sector_table_;
    allocation_table short_sector_table_;
    std::vector<sector_id> short_container_stream_;
};

class compound_document_writer_impl
{
public:
    compound_document_writer_impl(std::vector<byte> &bytes)
        : writer_(bytes)
    {
        sector_table_.sector_size(header_.sector_size());
        short_sector_table_.sector_size(header_.short_sector_size());
    }

    void write_sectors(const std::vector<byte> &data, directory_entry &/*entry*/)
    {
        const auto sector_size = sector_table_.sector_size();
        const auto num_sectors = data.size() / sector_size;

        for (auto i = std::size_t(0); i < num_sectors; ++i)
        {
            auto position = sector_size * i;
            auto current_sector_size = data.size() % sector_size;
            writer_.append(data, position, current_sector_size);
        }
    }

    void write_short_sectors(const std::vector<byte> &data, directory_entry &/*entry*/)
    {
        const auto sector_size = sector_table_.sector_size();
        const auto num_sectors = data.size() / sector_size;

        for (auto i = std::size_t(0); i < num_sectors; ++i)
        {
            auto position = sector_size * i;
            auto current_sector_size = data.size() % sector_size;
            writer_.append(data, position, current_sector_size);
        }
    }

    void write_stream(const std::u16string &name, const std::vector<byte> &data)
    {
        auto &entry = directory_.entry(name, true);
        
        if (entry.size < header_.threshold())
        {
            write_short_sectors(data, entry);
        }
        else
        {
            write_sectors(data, entry);
        }
    }

private:
    binary_writer writer_;
    directory_tree directory_;
    header header_;
    allocation_table sector_table_;
    allocation_table short_sector_table_;
    std::vector<sector_id> short_container_stream_;
};

compound_document_reader::compound_document_reader(const std::vector<std::uint8_t> &data)
    : d_(new compound_document_reader_impl(data))
{
}

compound_document_reader::~compound_document_reader()
{
}

std::vector<std::uint8_t> compound_document_reader::read_stream(const std::u16string &name) const
{
    return d_->read_stream(name);
}

compound_document_writer::compound_document_writer(std::vector<std::uint8_t> &data)
    : d_(new compound_document_writer_impl(data))
{
}

compound_document_writer::~compound_document_writer()
{
}

void compound_document_writer::write_stream(const std::u16string &name, const std::vector<std::uint8_t> &data)
{
    d_->write_stream(name, data);
}


} // namespace detail
} // namespace xlnt