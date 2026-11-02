#include "SeatTable.h"

#include <string.h>

void
CSeatTable::Clear(void)
{
	for(int i = 0; i < MAX_VEHICLES; i++){
		m_entries[i].id = MP_VEHICLE_NONE;
		memset(m_entries[i].seats, MP_PLAYER_NONE, sizeof(m_entries[i].seats));
	}
	m_next = 0;
}

CSeatTable::SEntry*
CSeatTable::Find(uint16_t id)
{
	if(id == MP_VEHICLE_NONE)
		return nullptr;
	for(int i = 0; i < MAX_VEHICLES; i++)
		if(m_entries[i].id == id)
			return &m_entries[i];
	return nullptr;
}

CSeatTable::SEntry*
CSeatTable::Add(uint16_t id)
{
	SEntry *e = Find(id);
	if(e || id == MP_VEHICLE_NONE)
		return e;
	for(int i = 0; i < MAX_VEHICLES; i++)
		if(m_entries[i].id == MP_VEHICLE_NONE){
			e = &m_entries[i];
			break;
		}
	if(e == nullptr){
		// Full: forget the oldest slot (round robin). Players only have so
		// many cars; this only bites if vehicles are never removed.
		e = &m_entries[m_next];
		m_next = (m_next + 1) % MAX_VEHICLES;
	}
	e->id = id;
	memset(e->seats, MP_PLAYER_NONE, sizeof(e->seats));
	return e;
}

void
CSeatTable::OnCreate(uint16_t id)
{
	Add(id);
}

void
CSeatTable::OnRemove(uint16_t id)
{
	SEntry *e = Find(id);
	if(e){
		e->id = MP_VEHICLE_NONE;
		memset(e->seats, MP_PLAYER_NONE, sizeof(e->seats));
	}
}

void
CSeatTable::ClearPlayer(uint8_t player)
{
	for(int i = 0; i < MAX_VEHICLES; i++)
		if(m_entries[i].id != MP_VEHICLE_NONE)
			for(int s = 0; s < MP_MAX_SEATS; s++)
				if(m_entries[i].seats[s] == player)
					m_entries[i].seats[s] = MP_PLAYER_NONE;
}

bool
CSeatTable::OnEnter(uint8_t from, uint16_t id, uint8_t seat)
{
	if(seat >= MP_MAX_SEATS || id == MP_VEHICLE_NONE || from >= MP_MAX_PLAYERS)
		return false;
	SEntry *e = Add(id);
	if(e->seats[seat] != MP_PLAYER_NONE && e->seats[seat] != from)
		return false;
	ClearPlayer(from);	// one seat at a time
	e->seats[seat] = from;
	return true;
}

void
CSeatTable::OnExit(uint8_t from, uint16_t id)
{
	if(id == MP_VEHICLE_NONE){
		ClearPlayer(from);
		return;
	}
	SEntry *e = Find(id);
	if(e)
		for(int s = 0; s < MP_MAX_SEATS; s++)
			if(e->seats[s] == from)
				e->seats[s] = MP_PLAYER_NONE;
}

uint8_t
CSeatTable::Occupant(uint16_t id, uint8_t seat) const
{
	if(seat >= MP_MAX_SEATS)
		return MP_PLAYER_NONE;
	SEntry *e = const_cast<CSeatTable*>(this)->Find(id);
	return e ? e->seats[seat] : MP_PLAYER_NONE;
}

bool
CSeatTable::Knows(uint16_t id) const
{
	return const_cast<CSeatTable*>(this)->Find(id) != nullptr;
}

int
CSeatTable::Count(void) const
{
	int n = 0;
	for(int i = 0; i < MAX_VEHICLES; i++)
		if(m_entries[i].id != MP_VEHICLE_NONE)
			n++;
	return n;
}
