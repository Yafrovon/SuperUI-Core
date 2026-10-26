/*
 * Copyright (C) 2005-2011 MaNGOS <http://getmangos.com/>
 * Copyright (C) 2009-2011 MaNGOSZero <https://github.com/mangos/zero>
 * Copyright (C) 2011-2016 Nostalrius <https://nostalrius.org>
 * Copyright (C) 2016-2017 Elysium Project <https://github.com/elysium-project>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef DBCSTORE_H
#define DBCSTORE_H

#include "DBCFileLoader.h"
#include <atomic>
#include <algorithm>
#include <cstring>

template<class T>
class DBCStorage
{
    typedef std::list<char*> StringPoolList;
    public:
        explicit DBCStorage(char const* f) : nCount(0), fieldCount(0), fmt(f), indexTable(nullptr), m_dataTable(nullptr) { }
        ~DBCStorage() { Clear(); }

        T const* LookupEntry(uint32 id) const { return (id>=nCount)?nullptr:indexTable[id]; }
        bool InsertEntry(uint32 id, T* data) { if (id >= nCount) return false; indexTable[id] = data; return true; }

        uint32  GetNumRows() const { return nCount; }
        char const* GetFormat() const { return fmt; }
        uint32 GetFieldCount() const { return fieldCount; }

        bool Load(char const* fn)
        {
            DBCFileLoader dbc;
            // Check if load was sucessful, only then continue
            if(!dbc.Load(fn, fmt))
                return false;

            fieldCount = dbc.GetCols();

            // load raw non-string data
            m_dataTable = (T*)dbc.AutoProduceData(fmt,nCount,(char**&)indexTable);

            // load strings from dbc data
            m_stringPoolList.push_back(dbc.AutoProduceStrings(fmt,(char*)m_dataTable));

            // error in dbc file at loading if nullptr
            return indexTable!=nullptr;
        }

        bool LoadStringsFrom(char const* fn)
        {
            // DBC must be already loaded using Load
            if(!indexTable)
                return false;

            DBCFileLoader dbc;
            // Check if load was successful, only then continue
            if(!dbc.Load(fn, fmt))
                return false;

            // load strings from another locale dbc data
            m_stringPoolList.push_back(dbc.AutoProduceStrings(fmt,(char*)m_dataTable));

            return true;
        }

        void Clear()
        {
            if (!indexTable)
                return;

            delete[] ((char*)indexTable);
            indexTable = nullptr;
            delete[] ((char*)m_dataTable);
            m_dataTable = nullptr;

            while(!m_stringPoolList.empty())
            {
                delete[] m_stringPoolList.front();
                m_stringPoolList.pop_front();
            }
            nCount = 0;
        }

        void EraseEntry(uint32 id) { indexTable[id] = nullptr; }

        // SuperUI (MSUIClient SPELL_CREATOR_PLAN.md D5): grow a LOADED store with the rows of a
        // re-read file whose ids it does not hold yet - the Spell Creator appends SpellRange /
        // SpellRadius / SpellCastTimes / SpellDuration rows (ids 1000+) while the server runs.
        // Append-only: an existing entry is never moved, changed or freed, and the superseded index
        // table is leaked on purpose (another thread may be reading it). Only formats without
        // strings may merge (the four index tables have none). Returns the rows added.
        uint32 MergeNewRowsFrom(char const* fn)
        {
            if (!indexTable || std::strchr(fmt, 's'))
                return 0;
            DBCFileLoader dbc;
            if (!dbc.Load(fn, fmt))
                return 0;
            uint32 fileCount = 0;
            T** fileIndex = nullptr;
            char* fileData = dbc.AutoProduceData(fmt, fileCount, (char**&)fileIndex);
            if (!fileIndex || !fileData)
                return 0;

            uint32 count = std::max(nCount, fileCount);
            T** merged = (T**)new char[count * sizeof(T*)];
            std::memset(merged, 0, count * sizeof(T*));
            uint32 added = 0;
            for (uint32 id = 0; id < count; ++id)
            {
                if (id < nCount && indexTable[id])
                    merged[id] = indexTable[id];
                else if (id < fileCount && fileIndex[id])
                {
                    merged[id] = fileIndex[id];
                    ++added;
                }
            }
            delete[] ((char*)fileIndex);
            if (!added)
            {
                delete[] ((char*)merged);
                delete[] fileData;
                return 0;
            }
            m_stringPoolList.push_back(fileData);   // the merged rows live here; freed by Clear()
            // Publish the table before the count: a reader that sees the new count sees the new
            // table. A new id is only asked for after this returns (.reload spell_template).
            indexTable = merged;
            std::atomic_thread_fence(std::memory_order_release);
            nCount = count;
            return added;
        }

    private:
        uint32 nCount;
        uint32 fieldCount;
        char const* fmt;
        T** indexTable;
        T* m_dataTable;
        StringPoolList m_stringPoolList;
};

#endif
