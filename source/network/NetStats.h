/* Copyright (C) 2023 Wildfire Games.
 * This file is part of 0 A.D.
 *
 * 0 A.D. is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * 0 A.D. is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with 0 A.D.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef INCLUDED_NETSTATS
#define INCLUDED_NETSTATS

#include "ps/ProfileViewer.h"

#include <mutex>
#include <vector>

typedef struct _ENetPeer ENetPeer;
typedef struct _ENetHost ENetHost;

/**
 * ENet connection statistics profiler table.
 *
 * Thread-safety:
 * - Must be constructed in the main thread (to match the profiler).
 * - In host mode, the host can be running in a separate thread;
 *   call LatchHostState from that thread periodically to safely
 *   update our displayed copy of the data.
 */
class CNetStatsTable : public AbstractProfileTable
{
	NONCOPYABLE(CNetStatsTable);
public:
	CNetStatsTable();
	CNetStatsTable(const ENetPeer* peer);

	CStr GetName() override;
	CStr GetTitle() override;
	size_t GetNumberRows() override;
	const std::vector<ProfileColumn>& GetColumns() override;
	CStr GetCellText(size_t row, size_t col) override;
	AbstractProfileTable* GetChild(size_t row) override;

	void LatchHostState(const ENetHost* host);

private:
	const ENetPeer* m_Peer;
	std::vector<ProfileColumn> m_ColumnDescriptions;

	std::mutex m_Mutex;
	std::vector<std::vector<CStr>> m_LatchedData; // protected by m_Mutex
};

#endif // INCLUDED_NETSTATS
