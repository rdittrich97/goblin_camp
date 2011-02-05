/* Copyright 2010-2011 Ilkka Halila
This file is part of Goblin Camp.

Goblin Camp is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Goblin Camp is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License 
along with Goblin Camp. If not, see <http://www.gnu.org/licenses/>.*/
#include "stdafx.hpp"

#include <boost/algorithm/string.hpp>

#include "StatusEffect.hpp"
#include "GCamp.hpp"
#include "Game.hpp"
#include "Attack.hpp"

//TODO: All this needs to be put into data files at some point

StatusEffect::StatusEffect(StatusEffectType typeval, int g, TCODColor col) : 
graphic(g),
	color(col),
	type(typeval),
	damageType(DAMAGE_BLUNT),
	visible(true),
	negative(true)
{
	//Initialize changes to nothing, ie. 100%
	for (int i = 0; i < STAT_COUNT; ++i) { statChanges[i] = 1.0; }
	for (int i = 0; i < RES_COUNT; ++i) { resistanceChanges[i] = 1.0; }
	damage.first = UPDATES_PER_SECOND;
	damage.second = 0;

	switch (type) {
	case HUNGER:
		name = "Hungry";
		graphic = TCOD_CHAR_ARROW_S;
		color = TCODColor::orange;
		cooldown = -1;
		break;

	case THIRST:
		name = "Thirsty";
		graphic = TCOD_CHAR_ARROW_S;
		color = TCODColor::blue;
		cooldown = -1;
		break;

	case PANIC:
		name = "Panicking";
		graphic = '!';
		color = TCODColor::white;
		cooldown = UPDATES_PER_SECOND * 5;
		break;

	case CONCUSSION:
		name = "Concussed";
		graphic = '?';
		color = TCODColor::grey;
		cooldown = UPDATES_PER_SECOND * 5;
		statChanges[MOVESPEED] = 0.5;
		statChanges[DODGE] = 0.5;
		break;

	case DROWSY:
		name = "Drowsy";
		graphic = 'z';
		color = TCODColor::lightGrey;
		cooldown = -1;
		statChanges[MOVESPEED] = 0.8;
		statChanges[DODGE] = 0.8;
		break;

	case SLEEPING:
		name = "Sleeping";
		graphic = 'Z';
		color = TCODColor::lightGrey;
		cooldown = UPDATES_PER_SECOND;
		break;

	case POISON:
		name = "Poisoned";
		graphic = '#';
		color = TCODColor::green;
		cooldown = MONTH_LENGTH*3;
		damage.second = 1;
		damageType = DAMAGE_POISON;
		break;

	case BLEEDING:
		name = "Bleeding";
		graphic = '#';
		color = TCODColor::red;
		cooldown = UPDATES_PER_SECOND * 4;
		damage.second = 4;
		damageType = DAMAGE_SLASH;
		break;

	case FLYING:
		name = "Flying";
		graphic = '"';
		color = TCODColor::lightBlue;
		cooldown = -1;
		negative=false;
		break;

	case BADSLEEP:
		name = "Sluggish";
		graphic = '-';
		color = TCODColor::grey;
		cooldown = MONTH_LENGTH*3;
		statChanges[MOVESPEED] = 0.75;
		statChanges[DODGE] = 0.75;
		resistanceChanges[POISON_RES] = 0.75;
		break;

	case RAGE:
		name = "Enraged";
		graphic = '!';
		color = TCODColor::red;
		cooldown = UPDATES_PER_SECOND * 7;
		statChanges[STRENGTH] = 2;
		statChanges[DODGE] = 0.5;
		negative=false;
		break;

	case SWIM:
		name = "Swimming";
		graphic = '~';
		color = TCODColor::lightBlue;
		cooldown = -1;
		statChanges[DODGE] = 0.0;
		statChanges[STRENGTH] = 0.75;
		negative=false;
		break;

	case EATING:
		name = "Eating";
		graphic = TCOD_CHAR_ARROW_N;
		color = TCODColor::orange;
		cooldown = -1;
		negative=false;
		break;

	case DRINKING:
		name = "Drinking";
		graphic = TCOD_CHAR_ARROW_N;
		color = TCODColor::blue;
		cooldown = -1;
		negative=false;
		break;

	case CARRYING:
		name = "Carrying item";
		cooldown = -1;
		negative=false;
		break;

	case WORKING:
		name = "Working";
		cooldown = -1;
		graphic = '+';
		color = TCODColor::grey;
		negative=false;
		break;

	case BURNING:
		name = "On fire!";
		cooldown = UPDATES_PER_SECOND * 10;
		graphic = '!';
		color = TCODColor::red;
		damage.second = 7;
		damageType = DAMAGE_FIRE;
		break;

	case CRACKEDSKULLEFFECT:
		name = "Cracked skull";
		cooldown = -1;
		graphic = 168;
		color = TCODColor::grey;
		visible = false;
		break;

	case INVIGORATED:
		name = "Invigorated";
		cooldown = MONTH_LENGTH * 3;
		graphic = 11;
		color = TCODColor(127,255,255);
		visible = false;
		statChanges[STRENGTH] = 1.25;
		statChanges[MOVESPEED] = 1.25;
		statChanges[DODGE] = 1.25;
		resistanceChanges[POISON_RES] = 1.25;
		negative=false;
		break;

	case DRUNK:
		name = "Drunk";
		cooldown = MONTH_LENGTH * 2;
		graphic = 63;
		color = TCODColor(218,255,127);
		break;

	case HEALING:
		name = "Healing";
		cooldown = MONTH_LENGTH;
		graphic = 241;
		color = TCODColor(0,255,0);
		damage.second = -10;
		damageType = DAMAGE_MAGIC;
		negative=false;
		break;

	default: break;
	}
	cooldownDefault = cooldown;
}

StatusEffectType StatusEffect::StringToStatusEffectType(std::string str) {
	if (boost::iequals(str, "hunger")) {
		return HUNGER;
	} else if (boost::iequals(str, "thirst")) {
		return THIRST;
	} else if (boost::iequals(str, "panic")) {
		return PANIC;
	} else if (boost::iequals(str, "concussion")) {
		return CONCUSSION;
	} else if (boost::iequals(str, "drowsy")) {
		return DROWSY;
	} else if (boost::iequals(str, "sleeping")) {
		return SLEEPING;
	} else if (boost::iequals(str, "poison")) {
		return POISON;
	} else if (boost::iequals(str, "bleeding")) {
		return BLEEDING;
	} else if (boost::iequals(str, "burning")) {
		return BURNING;
	} else if (boost::iequals(str, "sluggish")) {
		return BADSLEEP;
	} else if (boost::iequals(str, "invigorated")) {
		return INVIGORATED;
	} else if (boost::iequals(str, "drunk")) {
		return DRUNK;
	} else if (boost::iequals(str, "healing")) {
		return HEALING;
	}
	return HUNGER;
}

std::string StatusEffect::StatusEffectTypeToString(StatusEffectType type) {
	switch (type) {
	case HUNGER: return "hunger";
	case THIRST: return "thirst";
	case PANIC: return "panic";
	case CONCUSSION: return "concussion";
	case DROWSY: return "drowsy";
	case SLEEPING: return "sleeping";
	case POISON: return "poison";
	case BLEEDING: return "bleeding";
	case BURNING: return "burning";
	case BADSLEEP: return "sluggish";
	case INVIGORATED: return "invigorated";
	case DRUNK: return "drunk";
	case HEALING: return "healing";
	default: return "";
	}
}
