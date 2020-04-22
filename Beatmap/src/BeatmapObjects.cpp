#include "stdafx.h"
#include "BeatmapObjects.hpp"

// Object array sorting
void TObjectState<void>::SortArray(Vector<ObjectState*>& arr)
{
	arr.Sort([](const ObjectState* l, const ObjectState* r)
	{
		if(l->time == r->time)
		{
			// sort events on the same tick by their index
			if (l->type == ObjectType::Event && r->type == ObjectType::Event)
				return ((EventObjectState*)l)->interTickIndex < ((EventObjectState*)r)->interTickIndex;

			// Sort laser slams to come first
			bool ls = l->type == ObjectType::Laser && (((LaserObjectState*)l)->flags & LaserObjectState::flag_Instant);
			bool rs = r->type == ObjectType::Laser && (((LaserObjectState*)r)->flags & LaserObjectState::flag_Instant);
			return ls > rs;
		}
		return l->time < r->time;
	});
}
TObjectState<ObjectTypeData_Hold>* ObjectTypeData_Hold::GetRoot()
{
	TObjectState<ObjectTypeData_Hold>* ptr = (TObjectState<ObjectTypeData_Hold>*)this;
	while(ptr->prev)
		ptr = ptr->prev;
	return ptr;
}

TObjectState<ObjectTypeData_Laser>* ObjectTypeData_Laser::GetRoot()
{
	TObjectState<ObjectTypeData_Laser>* ptr = (TObjectState<ObjectTypeData_Laser>*)this;
	while(ptr->prev)
		ptr = ptr->prev;
	return ptr;
}
TObjectState<ObjectTypeData_Laser>* ObjectTypeData_Laser::GetTail()
{
	TObjectState<ObjectTypeData_Laser>* ptr = (TObjectState<ObjectTypeData_Laser>*)this;
	while(ptr->next)
		ptr = ptr->next;
	return ptr;
}
float ObjectTypeData_Laser::GetDirection() const
{
	return Math::Sign(points[1] - points[0]);
}
float ObjectTypeData_Laser::SamplePosition(MapTime time) const
{
	const LaserObjectState* state = (LaserObjectState*)this;
	while(state->next && (state->time + state->duration) < time)
	{
		state = state->next;
	}
	float f = Math::Clamp((float)(time - state->time) / (float)Math::Max(1, state->duration), 0.0f, 1.0f);
	return (state->points[1] - state->points[0]) * f + state->points[0];
}

float ObjectTypeData_Laser::ConvertToNormalRange(float inputRange)
{
	return (inputRange + 0.5f) * 0.5f;
}
float ObjectTypeData_Laser::ConvertToExtendedRange(float inputRange)
{
	return inputRange * 2.0f - 0.5f;
}

MapTime ObjectTypeData_Laser::GetTimeToDirectionChange(MapTime currentTime, MapTime maxDelta)
{
	const LaserObjectState* state = (LaserObjectState*)this;
	MapTime result = -1;
	LaserObjectState* n = state->next;
	float currentDir = state->GetDirection();

	//upcoming changes
	while (n) {
		if (n->time - currentTime > maxDelta)
			break;
		if (n->GetDirection() != currentDir) {
			result = Math::Max(0, n->time - currentTime);
			break;
		}
		n = n->next;
	}

	//previous changes
	n = state->prev;
	while (n) {
		MapTime changeTime = n->time + n->duration;
		if (currentTime - changeTime > maxDelta)
			break;
		if (n->GetDirection() != currentDir) {
			if (result == -1)
				result = Math::Max(currentTime - changeTime, 0);
			else
				result = Math::Min(Math::Max(currentTime - changeTime, 0), result);

			break;
		}
		n = n->prev;
	}
	return result;
}

// Enum OR, AND
TrackRollBehaviour operator|(const TrackRollBehaviour& l, const TrackRollBehaviour& r)
{
	return (TrackRollBehaviour)((uint8)l | (uint8)r);
}
TrackRollBehaviour operator&(const TrackRollBehaviour& l, const TrackRollBehaviour& r)
{
	return (TrackRollBehaviour)((uint8)l & (uint8)r);
}