/* Copyright (C) 2010 Wildfire Games.
 * This file is part of 0 A.D.
 *
 * 0 A.D. is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * 0 A.D. is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with 0 A.D.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef INCLUDED_NETSTATS
#define INCLUDED_NETSTATS

#include "ps/ProfileViewer.h"

typedef struct _ENetPeer ENetPeer;
typedef struct _ENetHost ENetHost;

class CNetStatsTable : public AbstractProfileTable
{
	NONCOPYABLE(CNetStatsTable);
public:
	CNetStatsTable(const ENetHost* host);
	CNetStatsTable(const ENetPeer* peer);

	virtual CStr GetName();
	virtual CStr GetTitle();
	virtual size_t GetNumberRows();
	virtual const std::vector<ProfileColumn>& GetColumns();
	virtual CStr GetCellText(size_t row, size_t col);
	virtual AbstractProfileTable* GetChild(size_t row);

private:
	const ENetHost* m_Host;
	const ENetPeer* m_Peer;
	std::vector<ProfileColumn> m_ColumnDescriptions;
};

#endif // INCLUDED_NETSTATS
