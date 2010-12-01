/* Copyright 2010 Ilkka Halila
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

#include <cstdlib>
#include <string>
#include <boost/serialization/split_member.hpp>
#include <boost/thread/thread.hpp>
#include <boost/multi_array.hpp>
#include <boost/format.hpp>
#include <boost/algorithm/string.hpp>
#include <libtcod.hpp>
#ifdef DEBUG
#include <iostream>
#endif

#include "Random.hpp"
#include "NPC.hpp"
#include "Coordinate.hpp"
#include "JobManager.hpp"
#include "GCamp.hpp"
#include "Game.hpp"
#include "Announce.hpp"
#include "Logger.hpp"
#include "Map.hpp"
#include "StatusEffect.hpp"
#include "Camp.hpp"
#include "Stockpile.hpp"

SkillSet::SkillSet() {
	for (int i = 0; i < SKILLAMOUNT; ++i) { skills[i] = 0; }
}

int SkillSet::operator()(Skill skill) {return skills[skill];}
void SkillSet::operator()(Skill skill, int value) {skills[skill] = value;}

std::map<std::string, NPCType> NPC::NPCTypeNames = std::map<std::string, NPCType>();
std::vector<NPCPreset> NPC::Presets = std::vector<NPCPreset>();

NPC::NPC(Coordinate pos, boost::function<bool(boost::shared_ptr<NPC>)> findJob,
	boost::function<void(boost::shared_ptr<NPC>)> react) : Entity(),
	type(0),
	timeCount(0),
	taskIndex(0),
	orderIndex(0),
	pathIndex(0),
	nopath(false),
	findPathWorking(false),
	timer(0),
	nextMove(0),
	lastMoveResult(TASKCONTINUE),
	run(true),
	taskBegun(false),
	expert(false),
	carried(boost::weak_ptr<Item>()),
	mainHand(boost::weak_ptr<Item>()),
	offHand(boost::weak_ptr<Item>()),
	armor(boost::weak_ptr<Item>()),
	thirst(0),
	hunger(0),
	weariness(0),
	thinkSpeed(UPDATES_PER_SECOND / 5), //Think 5 times a second
	statusEffects(std::list<StatusEffect>()),
	statusEffectIterator(statusEffects.end()),
	statusGraphicCounter(0),
	health(100),
	maxHealth(100),
	foundItem(boost::weak_ptr<Item>()),
	inventory(boost::shared_ptr<Container>(new Container(pos, 0, 10, -1))),
	needsNutrition(false),
	needsSleep(false),
	hasHands(false),
	aggressive(false),
	coward(false),
	aggressor(boost::weak_ptr<NPC>()),
	dead(false),
	squad(boost::weak_ptr<Squad>()),
	attacks(std::list<Attack>()),
	addedTasksToCurrentJob(0),
	FindJob(findJob),
	React(react),
	escaped(false)
{
	while (!Map::Inst()->Walkable(pos.X(),pos.Y())) {
		pos.X(pos.X()+1);
		pos.Y(pos.Y()+1);
	}
	inventory->SetInternal();
	Position(pos,true);

	thirst = thirst - (THIRST_THRESHOLD / 2) + Random::Generate(THIRST_THRESHOLD - 1);
	hunger = hunger - (HUNGER_THRESHOLD / 2) + Random::Generate(HUNGER_THRESHOLD - 1);
	weariness = weariness - (WEARY_THRESHOLD / 2) + Random::Generate(WEARY_THRESHOLD - 1);

	path = new TCODPath(Map::Inst()->Width(), Map::Inst()->Height(), Map::Inst(), (void*)this);

	for (int i = 0; i < STAT_COUNT; ++i) {baseStats[i] = 0; effectiveStats[i] = 0;}
	for (int i = 0; i < RES_COUNT; ++i) {baseResistances[i] = 0; effectiveResistances[i] = 0;}
}

NPC::~NPC() {
	Map::Inst()->NPCList(x, y)->erase(uid);
	if (squad.lock()) squad.lock()->Leave(uid);

	if (boost::iequals(NPC::NPCTypeToString(type), "orc")) Game::Inst()->OrcCount(-1);
	else if (boost::iequals(NPC::NPCTypeToString(type), "goblin")) Game::Inst()->GoblinCount(-1);
	else if (NPC::Presets[type].tags.find("localwildlife") != NPC::Presets[type].tags.end()) Game::Inst()->PeacefulFaunaCount(-1);

	delete path;
}

void NPC::Position(Coordinate pos, bool firstTime) {
	if (!firstTime) {
		Map::Inst()->MoveTo(pos.X(), pos.Y(), uid);
		Map::Inst()->MoveFrom(x, y, uid);
		x = pos.X();
		y = pos.Y();
	} else {
		Map::Inst()->MoveTo(pos.X(), pos.Y(), uid);
		x = pos.X();
		y = pos.Y();
	}
	inventory->Position(pos);
}

void NPC::Position(Coordinate pos) { Position(pos, false); }

Task* NPC::currentTask() { return jobs.empty() ? 0 : &(jobs.front()->tasks[taskIndex]); }
Task* NPC::nextTask() { 
	if (!jobs.empty()) {
		if ((signed int)jobs.front()->tasks.size() > taskIndex+1) {
			return &jobs.front()->tasks[taskIndex+1];
		}
	}
	return 0;
}

boost::weak_ptr<Job> NPC::currentJob() { return jobs.empty() ? boost::weak_ptr<Job>() : boost::weak_ptr<Job>(jobs.front()); }

void NPC::TaskFinished(TaskResult result, std::string msg) {
#ifdef DEBUG
	if (msg.size() > 0) {
		std::cout<<name<<":"<<msg<<"\n";
	}
#endif
	RemoveEffect(EATING);
	RemoveEffect(DRINKING);
	if (jobs.size() > 0) {
		if (result == TASKSUCCESS) {
			if (++taskIndex >= (signed int)jobs.front()->tasks.size()) {
				jobs.front()->Complete();
				jobs.pop_front();
				taskIndex = 0;
				foundItem = boost::weak_ptr<Item>();
				addedTasksToCurrentJob = 0;
			}
		} else {
			//Remove any tasks this NPC added onto the front before sending it back to the JobManager
			for (int i = 0; i < addedTasksToCurrentJob; ++i) {
				if (!jobs.front()->tasks.empty()) jobs.front()->tasks.erase(jobs.front()->tasks.begin());
			}
			if (!jobs.front()->internal) JobManager::Inst()->CancelJob(jobs.front(), msg, result);
			jobs.pop_front();
			taskIndex = 0;
			DropItem(carried);
			carried.reset();
			foundItem = boost::weak_ptr<Item>();
			addedTasksToCurrentJob = 0;
		}
	}
	taskBegun = false;
}

void NPC::HandleThirst() {
	Coordinate tmpCoord;
	bool found = false;

	for (std::deque<boost::shared_ptr<Job> >::iterator jobIter = jobs.begin(); jobIter != jobs.end(); ++jobIter) {
		if ((*jobIter)->name.find("Drink") != std::string::npos) found = true;
	}
	if (!found) {
		boost::weak_ptr<Item> item = Game::Inst()->FindItemByCategoryFromStockpiles(Item::StringToItemCategory("Drink"), Position());
		if (!item.lock()) {tmpCoord = Game::Inst()->FindWater(Position());}
		if (!item.lock() && tmpCoord.X() == -1) { //Nothing to drink!
			//:ohdear:
		} else { //Something to drink!
			boost::shared_ptr<Job> newJob(new Job("Drink", MED, 0, !expert));
			newJob->internal = true;

			if (item.lock()) {
				newJob->ReserveEntity(item);
				newJob->tasks.push_back(Task(MOVE,item.lock()->Position()));
				newJob->tasks.push_back(Task(TAKE,item.lock()->Position(), item));
				newJob->tasks.push_back(Task(DRINK));
				jobs.push_back(newJob);
				run = true;
			} else {
				for (int ix = tmpCoord.X()-1; ix <= tmpCoord.X()+1; ++ix) {
					for (int iy = tmpCoord.Y()-1; iy <= tmpCoord.Y()+1; ++iy) {
						if (Map::Inst()->Walkable(ix,iy,(void*)this)) {
							newJob->tasks.push_back(Task(MOVE, Coordinate(ix,iy)));
							goto CONTINUEDRINKBLOCK;
						}
					}
				}
CONTINUEDRINKBLOCK:
				newJob->tasks.push_back(Task(DRINK, tmpCoord));
				jobs.push_back(newJob);
				run = true;
			}
		}
	}
}

void NPC::HandleHunger() {
	Coordinate tmpCoord;
	bool found = false;

	for (std::deque<boost::shared_ptr<Job> >::iterator jobIter = jobs.begin(); jobIter != jobs.end(); ++jobIter) {
		if ((*jobIter)->name.find("Eat") != std::string::npos) found = true;
	}
	if (!found) {
		boost::weak_ptr<Item> item = Game::Inst()->FindItemByCategoryFromStockpiles(Item::StringToItemCategory("Prepared food"), Position());
		if (!item.lock()) {item = Game::Inst()->FindItemByCategoryFromStockpiles(Item::StringToItemCategory("Food"), Position());}
		if (!item.lock()) { //Nothing to eat!
			if (hunger > 48000) { //Nearing death
				Game::Inst()->FindNearbyNPCs(boost::static_pointer_cast<NPC>(shared_from_this()));
				boost::shared_ptr<NPC> weakest;
				for (std::list<boost::weak_ptr<NPC> >::iterator npci = nearNpcs.begin(); npci != nearNpcs.end(); ++npci) {
					if (npci->lock() && (!weakest || npci->lock()->health < weakest->health)) {
						weakest = npci->lock();
					}
				}

				if (weakest) { //Found a creature nearby, eat it
					boost::shared_ptr<Job> newJob(new Job("Eat", HIGH, 0, !expert));
					newJob->internal = true;
					newJob->tasks.push_back(Task(GETANGRY));
					newJob->tasks.push_back(Task(KILL, weakest->Position(), weakest, 0, 1));
					newJob->tasks.push_back(Task(EAT));
					newJob->tasks.push_back(Task(CALMDOWN));
					jobs.push_back(newJob);
					run = true;
				}				
			}
		} else { //Something to eat!
			boost::shared_ptr<Job> newJob(new Job("Eat", MED, 0, !expert));
			newJob->internal = true;

			newJob->ReserveEntity(item);
			newJob->tasks.push_back(Task(MOVE,item.lock()->Position()));
			newJob->tasks.push_back(Task(TAKE,item.lock()->Position(), item));
			newJob->tasks.push_back(Task(EAT));
			jobs.push_back(newJob);
			run = true;
		}
	}
}

void NPC::HandleWeariness() {
	bool found = false;
	for (std::deque<boost::shared_ptr<Job> >::iterator jobIter = jobs.begin(); jobIter != jobs.end(); ++jobIter) {
		if ((*jobIter)->name.find("Sleep") != std::string::npos) found = true;
	}
	if (!found) {
		boost::weak_ptr<Construction> wbed = Game::Inst()->FindConstructionByTag(BED);
		boost::shared_ptr<Job> sleepJob(new Job("Sleep"));
		sleepJob->internal = true;
		if (!expert && mainHand.lock()) { //Menial job doers may wield a tool
			sleepJob->tasks.push_back(Task(UNWIELD));
			sleepJob->tasks.push_back(Task(TAKE));
			sleepJob->tasks.push_back(Task(STOCKPILEITEM));
		}
		if (boost::shared_ptr<Construction> bed = wbed.lock()) {
			run = true;
			sleepJob->ReserveEntity(bed);
			sleepJob->tasks.push_back(Task(MOVE, bed->Position()));
			sleepJob->tasks.push_back(Task(SLEEP, bed->Position(), bed));
			jobs.push_back(sleepJob);
			return;
		}
		sleepJob->tasks.push_back(Task(SLEEP, Position()));
		jobs.push_back(sleepJob);
	}
}

void NPC::Update() {
	if (Map::Inst()->NPCList(x,y)->size() > 1) _bgcolor = TCODColor::darkGrey;
	else _bgcolor = TCODColor::black;

	UpdateStatusEffects();
	//Apply armor effects if present
	if (boost::shared_ptr<Item> arm = armor.lock()) {
		for (int i = 0; i < RES_COUNT; ++i) {
			effectiveResistances[i] += arm->Resistance(i);
		}
	}

	if (!HasEffect(FLYING) && effectiveStats[MOVESPEED] > 0) effectiveStats[MOVESPEED] = std::max(1, effectiveStats[MOVESPEED]-Map::Inst()->GetMoveModifier(x,y));
	effectiveStats[MOVESPEED] = std::max(1, effectiveStats[MOVESPEED]-bulk);
	

	if (needsNutrition) {
		++thirst; ++hunger;

		if (thirst >= THIRST_THRESHOLD) AddEffect(THIRST);
		else RemoveEffect(THIRST);
		if (hunger >= HUNGER_THRESHOLD) AddEffect(HUNGER);
		else RemoveEffect(HUNGER);

		if (thirst > THIRST_THRESHOLD && Random::Generate(UPDATES_PER_SECOND * 5 - 1) == 0) {
			HandleThirst();
		} else if (thirst > THIRST_THRESHOLD * 2) Kill();
		if (hunger > HUNGER_THRESHOLD && Random::Generate(UPDATES_PER_SECOND * 5 - 1) == 0) {
			HandleHunger();
		} else if (hunger > 72000) Kill();
	}

	if (needsSleep) {
		++weariness;

		if (weariness >= WEARY_THRESHOLD) { 
			AddEffect(DROWSY);
			HandleWeariness();
		} else RemoveEffect(DROWSY);
	}

	if (boost::shared_ptr<WaterNode> water = Map::Inst()->GetWater(x,y).lock()) {
		boost::shared_ptr<Construction> construct = Game::Inst()->GetConstruction(Map::Inst()->GetConstruction(x,y)).lock();
		if (water->Depth() > WALKABLE_WATER_DEPTH && (!construct || !construct->HasTag(BRIDGE) || !construct->Built())) {
			AddEffect(SWIM);
		} else { RemoveEffect(SWIM); }
	} else { RemoveEffect(SWIM); }

	for (std::list<Attack>::iterator attacki = attacks.begin(); attacki != attacks.end(); ++attacki) {
		attacki->Update();
	}

	if (Random::Generate(UPDATES_PER_SECOND - 1) == 0 && health < maxHealth) ++health;
	if (faction == 0 && Random::Generate(MONTH_LENGTH - 1) == 0) Game::Inst()->CreateFilth(Position());
}

void NPC::UpdateStatusEffects() {

	for (int i = 0; i < STAT_COUNT; ++i) {
		effectiveStats[i] = baseStats[i];
	}
	for (int i = 0; i < STAT_COUNT; ++i) {
		effectiveResistances[i] = baseResistances[i];
	}
	++statusGraphicCounter;
	for (std::list<StatusEffect>::iterator statusEffectI = statusEffects.begin(); statusEffectI != statusEffects.end(); ++statusEffectI) {
		//Apply effects to stats
		for (int i = 0; i < STAT_COUNT; ++i) {
			effectiveStats[i] = (int)(effectiveStats[i] * statusEffectI->statChanges[i]);
		}
		for (int i = 0; i < STAT_COUNT; ++i) {
			effectiveResistances[i] = (int)(effectiveResistances[i] * statusEffectI->resistanceChanges[i]);
		}

		if (statusEffectI->damage.second > 0 && --statusEffectI->damage.first <= 0) {
			statusEffectI->damage.first = UPDATES_PER_SECOND;
			health -= statusEffectI->damage.second;
			if (statusEffectI->bleed) Game::Inst()->CreateBlood(Position());
		}

		//Remove the statuseffect if its cooldown has run out
		if (statusEffectI->cooldown > 0 && --statusEffectI->cooldown == 0) {
			if (statusEffectI == statusEffectIterator) {
				++statusEffectIterator;
				statusGraphicCounter = 0;
			}
			statusEffectI = statusEffects.erase(statusEffectI);
			if (statusEffectIterator == statusEffects.end()) statusEffectIterator = statusEffects.begin();
		}
	}
	
	if (statusGraphicCounter > 10) {
		statusGraphicCounter = 0;
		if (statusEffectIterator != statusEffects.end()) ++statusEffectIterator;
		else statusEffectIterator = statusEffects.begin();
	}

}

AiThink NPC::Think() {
	Coordinate tmpCoord;
	int tmp;
	
	UpdateVelocity();

	lastMoveResult = Move(lastMoveResult);

	if (velocity == 0) {
		timeCount += thinkSpeed; //Can't think while hurtling through the air, sorry
	} else if (!jobs.empty()) {
		TaskFinished(TASKFAILFATAL, "Flying through the air");
		JobManager::Inst()->NPCNotWaiting(uid);
	}

	while (timeCount > UPDATES_PER_SECOND) {

		if (Random::GenerateBool()) React(boost::static_pointer_cast<NPC>(shared_from_this()));

		if (aggressor.lock()) {
			JobManager::Inst()->NPCNotWaiting(uid);
			if (Game::Adjacent(Position(), aggressor)) {
				if (currentTask() && currentTask()->action == KILL) Hit(aggressor, currentTask()->flags != 0);
				else Hit(aggressor);
			}
			if (Random::Generate(9) <= 3 && Distance(Position(), aggressor.lock()->Position()) > LOS_DISTANCE) {
				aggressor.reset();
				TaskFinished(TASKFAILFATAL, "Target lost");
			}
		}

		timeCount -= UPDATES_PER_SECOND;
		if (!jobs.empty()) {
			switch(currentTask()->action) {
			case MOVE:
				if (!Map::Inst()->Walkable(currentTarget().X(), currentTarget().Y(), (void*)this)) {
					TaskFinished(TASKFAILFATAL, "(MOVE)Target unwalkable");
					break;
				}
				if ((signed int)x == currentTarget().X() && (signed int)y == currentTarget().Y()) {
					TaskFinished(TASKSUCCESS);
					break;
				}
				if (!taskBegun) { findPath(currentTarget()); taskBegun = true; lastMoveResult = TASKCONTINUE;}

				if (lastMoveResult == TASKFAILFATAL || lastMoveResult == TASKFAILNONFATAL) {
					TaskFinished(lastMoveResult, std::string("(MOVE)Could not find path to target")); break;
				} else if (lastMoveResult == PATHEMPTY) {
					if (!((signed int)x == currentTarget().X() && (signed int)y == currentTarget().Y())) {
						TaskFinished(TASKFAILFATAL, std::string("(MOVE)No path to target")); break;
					}
				}
				break;

			case MOVENEAR:
				/*MOVENEAR first figures out our "real" target, which is a tile near
				to our current target. Near means max 5 tiles away, visible and
				walkable. Once we have our target we can actually switch over
				to a normal MOVE task. In case we can't find a visible tile,
				we'll allow a non LOS one*/
				{bool checkLOS = true;
				for (int i = 0; i < 2; ++i) {
					tmp = 0;
					while (tmp++ < 10) {
						int tarX = Random::Generate(-5, 5) + currentTarget().X();
						int tarY = Random::Generate(-5, 5) + currentTarget().Y();
						if (tarX < 0) tarX = 0;
						if (tarX >= Map::Inst()->Width()) tarX = Map::Inst()->Width()-1;
						if (tarY < 0) tarY = 0;
						if (tarY >= Map::Inst()->Height()) tarY = Map::Inst()->Height()-1;
						if (Map::Inst()->Walkable(tarX, tarY, (void *)this) && !Map::Inst()->IsUnbridgedWater(tarX, tarY)) {
							if (!checkLOS || (checkLOS && 
								Map::Inst()->LineOfSight(tarX, tarY, currentTarget().X(), currentTarget().Y()))) {
								currentJob().lock()->tasks[taskIndex] = Task(MOVE, Coordinate(tarX, tarY));
								goto MOVENEARend;
							}
						}
					}
					checkLOS = !checkLOS;
				}}
				//If we got here we couldn't find a near coordinate
				TaskFinished(TASKFAILFATAL, "(MOVENEAR)Couldn't find NEAR coordinate");
MOVENEARend:
				break;


			case MOVEADJACENT:
				if (currentEntity().lock()) {
					if (Game::Adjacent(Position(), currentEntity())) {
						TaskFinished(TASKSUCCESS);
						break;
					}
				} else {
					if (Game::Adjacent(Position(), currentTarget())) {
						TaskFinished(TASKSUCCESS);
						break;
					}
				}
				if (!taskBegun) {
					if (currentEntity().lock()) tmpCoord = Game::Inst()->FindClosestAdjacent(Position(), currentEntity());
					else tmpCoord = Game::Inst()->FindClosestAdjacent(Position(), currentTarget());
					if (tmpCoord.X() >= 0) {
						findPath(tmpCoord);
					} else { TaskFinished(TASKFAILFATAL, std::string("(MOVEADJACENT)No walkable adjacent tiles")); break; }
					taskBegun = true;
					lastMoveResult = TASKCONTINUE;
				}
				if (lastMoveResult == TASKFAILFATAL || lastMoveResult == TASKFAILNONFATAL) { TaskFinished(lastMoveResult, std::string("Could not find path to target")); break; }
				else if (lastMoveResult == PATHEMPTY) {
					TaskFinished(TASKFAILFATAL, "(MOVEADJACENT)PATHEMPTY");
				}
				break;

			case WAIT:
				if (++timer > currentTarget().X()) { timer = 0; TaskFinished(TASKSUCCESS); }
				break;

			case BUILD:
				if (Game::Adjacent(Position(), currentEntity())) {
					tmp = boost::static_pointer_cast<Construction>(currentEntity().lock())->Build();
					if (tmp > 0) {
						Announce::Inst()->AddMsg((boost::format("%s completed") % currentEntity().lock()->Name()).str(), TCODColor::white, currentEntity().lock()->Position());
						TaskFinished(TASKSUCCESS);
						break;
					} else if (tmp == BUILD_NOMATERIAL) {
						TaskFinished(TASKFAILFATAL, "(BUILD)Missing materials");
						break;
					}
				} else {
					TaskFinished(TASKFAILFATAL, "(BUILD)Not adjacent to target");
					break;
				}
				break;

			case TAKE:
				if (!currentEntity().lock()) { TaskFinished(TASKFAILFATAL, "(TAKE)No target entity"); break; }
				if (Position() == currentEntity().lock()->Position()) {
					if (boost::static_pointer_cast<Item>(currentEntity().lock())->ContainedIn().lock()) {
						boost::weak_ptr<Container> cont(boost::static_pointer_cast<Container>(boost::static_pointer_cast<Item>(currentEntity().lock())->ContainedIn().lock()));
						cont.lock()->RemoveItem(
							boost::static_pointer_cast<Item>(currentEntity().lock()));
					}
					PickupItem(boost::static_pointer_cast<Item>(currentEntity().lock()));
					TaskFinished(TASKSUCCESS);
					break;
				} else { TaskFinished(TASKFAILFATAL, "(TAKE)Item not found"); break; }
				break;

			case DROP:
				DropItem(carried);
				carried.reset();
				TaskFinished(TASKSUCCESS);
				break;

			case PUTIN:
				if (carried.lock()) {
					inventory->RemoveItem(carried);
					carried.lock()->Position(Position());
					if (!currentEntity().lock()) {
						TaskFinished(TASKFAILFATAL, "(PUTIN)Target does not exist");
						break;
					}
					if (!Game::Adjacent(Position(), currentEntity().lock()->Position())) {
						TaskFinished(TASKFAILFATAL, "(PUTIN)Not adjacent to container");
						break;
					}
					if (boost::dynamic_pointer_cast<Container>(currentEntity().lock())) {
						boost::shared_ptr<Container> cont = boost::static_pointer_cast<Container>(currentEntity().lock());
						if (!cont->AddItem(carried)) {
							TaskFinished(TASKFAILFATAL, "(PUTIN)Container full");
							break;
						}
						bulk -= carried.lock()->GetBulk();
					} else {
						TaskFinished(TASKFAILFATAL, "(PUTIN)Target not a container");
						break;
					}
				}
				carried.reset();
				TaskFinished(TASKSUCCESS);
				break;

			case DRINK: //Either we have an item target to drink, or a water tile
				if (carried.lock()) { //Drink from an item
					timer = boost::static_pointer_cast<OrganicItem>(carried.lock())->Nutrition();
					inventory->RemoveItem(carried);
					bulk -= carried.lock()->GetBulk();
					Game::Inst()->RemoveItem(carried);
					carried = boost::weak_ptr<Item>();
				} else { //Drink from a water tile
					if (std::abs((signed int)x - currentTarget().X()) <= 1 &&
						std::abs((signed int)y - currentTarget().Y()) <= 1) {
							if (boost::shared_ptr<WaterNode> water = Map::Inst()->GetWater(currentTarget().X(), currentTarget().Y()).lock()) {
								if (water->Depth() > DRINKABLE_WATER_DEPTH) {
									thirst -= (int)(THIRST_THRESHOLD / 10);
									AddEffect(DRINKING);
									if (thirst < 0) { 
										TaskFinished(TASKSUCCESS); 
									}
									break;
								}
							}
					}
					TaskFinished(TASKFAILFATAL, "(DRINK)Not enough water");
				}

				if (timer > 0) {
					AddEffect(DRINKING);
					thirst -= std::min((int)(THIRST_THRESHOLD / 5), timer);
					timer -= (int)(THIRST_THRESHOLD / 5);
					if (timer <= 0) {
						timer = 0;
						TaskFinished(TASKSUCCESS);
					}
				}
				break;

			case EAT:
				if (carried.lock()) {
					//Set the nutrition to the timer variable
					timer = boost::static_pointer_cast<OrganicItem>(carried.lock())->Nutrition();
					inventory->RemoveItem(carried);
					bulk -= carried.lock()->GetBulk();

					for (std::list<ItemType>::iterator fruiti = Item::Presets[carried.lock()->Type()].fruits.begin(); fruiti != Item::Presets[carried.lock()->Type()].fruits.end(); ++fruiti) {
						Game::Inst()->CreateItem(Position(), *fruiti, true);
					}

					Game::Inst()->RemoveItem(carried);
				} else if (timer == 0) { //Look in all adjacent tiles for anything edible
					for (int ix = (int)x - 1; ix <= (int)x + 1; ++ix) {
						for (int iy = (int)y - 1; iy <= (int)y + 1; ++iy) {
							if (ix >= 0 && ix < Map::Inst()->Width() && iy >= 0 && iy < Map::Inst()->Height()) {
								for (std::set<int>::iterator itemi = Map::Inst()->ItemList(ix, iy)->begin();
									itemi != Map::Inst()->ItemList(ix, iy)->end(); ++itemi) {
										boost::shared_ptr<Item> item = Game::Inst()->GetItem(*itemi).lock();
										if (item && (item->IsCategory(Item::StringToItemCategory("food")) ||
											item->IsCategory(Item::StringToItemCategory("corpse") ))) {
												jobs.front()->ReserveEntity(item);
												jobs.front()->tasks.push_back(Task(MOVE, item->Position()));
												jobs.front()->tasks.push_back(Task(TAKE, item->Position(), item));
												jobs.front()->tasks.push_back(Task(EAT));
												goto CONTINUEEAT;
										}
								}

							}
						}
					}
				} 
				
				if (timer > 0) {
					AddEffect(EATING);
					hunger -= std::min(5000, timer);
					timer -= 5000;
					if (timer <= 0) {
						timer = 0;
						TaskFinished(TASKSUCCESS);
					}
					break;
				}
CONTINUEEAT:
				carried = boost::weak_ptr<Item>();
				TaskFinished(TASKSUCCESS);
				break;

			case FIND:
				foundItem = Game::Inst()->FindItemByCategoryFromStockpiles(currentTask()->item, currentTask()->target, currentTask()->flags);
				if (!foundItem.lock()) {
					TaskFinished(TASKFAILFATAL, "(FIND)Failed"); 
					break;
				} else {
					if (faction == 0) currentJob().lock()->ReserveEntity(foundItem);
					TaskFinished(TASKSUCCESS);
					break;
				}

			case USE:
				if (currentEntity().lock() && boost::dynamic_pointer_cast<Construction>(currentEntity().lock())) {
					tmp = boost::static_pointer_cast<Construction>(currentEntity().lock())->Use();
					if (tmp >= 100) {
						TaskFinished(TASKSUCCESS);
					} else if (tmp < 0) {
						TaskFinished(TASKFAILFATAL, "(USE)Can not use (tmp<0)"); break;
					}
				} else { TaskFinished(TASKFAILFATAL, "(USE)Attempted to use non-construct"); break; }
				break;

			case HARVEST:
				if (carried.lock()) {
					bool stockpile = false;
					if (nextTask() && nextTask()->action == STOCKPILEITEM) stockpile = true;

					boost::shared_ptr<Item> plant = carried.lock();
					inventory->RemoveItem(carried);
					bulk -= plant->GetBulk();
					carried = boost::weak_ptr<Item>();

					for (std::list<ItemType>::iterator fruiti = Item::Presets[plant->Type()].fruits.begin(); fruiti != Item::Presets[plant->Type()].fruits.end(); ++fruiti) {
						if (stockpile) {
							int item = Game::Inst()->CreateItem(Position(), *fruiti, false);
							PickupItem(Game::Inst()->GetItem(item));
							stockpile = false;
						} else {
							Game::Inst()->CreateItem(Position(), *fruiti, true);
						}
					}

					Game::Inst()->RemoveItem(plant);

					TaskFinished(TASKSUCCESS);
					break;
				} else {
					inventory->RemoveItem(carried);
					carried = boost::weak_ptr<Item>();
					TaskFinished(TASKFAILFATAL, "(HARVEST)Carrying nonexistant item");
					break;
				}

			case FELL:
				if (boost::shared_ptr<NatureObject> tree = boost::static_pointer_cast<NatureObject>(currentEntity().lock())) {
					tmp = tree->Fell();
					if (tmp <= 0) {
						bool stockpile = false;
						if (nextTask() && nextTask()->action == STOCKPILEITEM) stockpile = true;
						for (std::list<ItemType>::iterator iti = NatureObject::Presets[tree->Type()].components.begin(); iti != NatureObject::Presets[tree->Type()].components.end(); ++iti) {
							if (stockpile) {
								int item = Game::Inst()->CreateItem(tree->Position(), *iti, false);
								DropItem(carried);
								PickupItem(Game::Inst()->GetItem(item));
								stockpile = false;
							} else {
								Game::Inst()->CreateItem(tree->Position(), *iti, true);
							}
						}
						Game::Inst()->RemoveNatureObject(tree);
						TaskFinished(TASKSUCCESS);
						break;
					}
					//Job underway
					break;
				}
				TaskFinished(TASKFAILFATAL, "(FELL) No NatureObject to fell");
				break;

			case HARVESTWILDPLANT:
				if (boost::shared_ptr<NatureObject> plant = boost::static_pointer_cast<NatureObject>(currentEntity().lock())) {
					tmp = plant->Harvest();
					if (tmp <= 0) {
						bool stockpile = false;
						if (nextTask() && nextTask()->action == STOCKPILEITEM) stockpile = true;
						for (std::list<ItemType>::iterator iti = NatureObject::Presets[plant->Type()].components.begin(); iti != NatureObject::Presets[plant->Type()].components.end(); ++iti) {
							if (stockpile) {
								int item = Game::Inst()->CreateItem(plant->Position(), *iti, false);
								DropItem(carried);
								PickupItem(Game::Inst()->GetItem(item));			
								stockpile = false;
							} else {
								Game::Inst()->CreateItem(plant->Position(), *iti, true);
							}
						}
						Game::Inst()->RemoveNatureObject(plant);
						TaskFinished(TASKSUCCESS);
						break;
					}
					//Job underway
					break;
				}
				TaskFinished(TASKFAILFATAL, "(HARVESTWILDPLANT)Harvest target doesn't exist");
				break;

			case KILL:
				//The reason KILL isn't a combination of MOVEADJACENT and something else is that the other moving actions
				//assume their target isn't a moving one
				if (!currentEntity().lock()) {
					TaskFinished(TASKSUCCESS);
					break;
				}

				if (Game::Adjacent(Position(), currentEntity())) {
					Hit(currentEntity(), currentTask()->flags != 0);
					break;
				} else if (currentTask()->flags == 0 && WieldingRangedWeapon() && quiver.lock() && 
					!quiver.lock()->empty()) {
					FireProjectile(currentEntity());
					break;
				}

				if (!taskBegun || Random::Generate(UPDATES_PER_SECOND * 2 - 1) == 0) { //Repath every ~2 seconds
					tmpCoord = Game::Inst()->FindClosestAdjacent(Position(), currentEntity());
					if (tmpCoord.X() >= 0) {
						findPath(tmpCoord);
					}
					taskBegun = true;
					lastMoveResult = TASKCONTINUE;
				}

				if (lastMoveResult == TASKFAILFATAL || lastMoveResult == TASKFAILNONFATAL) { TaskFinished(lastMoveResult, std::string("(KILL)Could not find path to target")); break; }
				else if (lastMoveResult == PATHEMPTY) {
					tmpCoord = Game::Inst()->FindClosestAdjacent(Position(), currentEntity());
					if (tmpCoord.X() >= 0) {
						findPath(tmpCoord);
					}
				}
				break;

			case FLEEMAP:
				if (x == 0 || x == Map::Inst()->Width()-1 ||
					y == 0 || y == Map::Inst()->Height()-1) {
						//We are at the edge, escape!
						Escape();
						return AIMOVE;
				}

				//Find the closest edge and change into a MOVE task and a new FLEEMAP task
				//Unfortunately this assumes that FLEEMAP is the last task in a job,
				//which might not be.
				tmp = std::abs((signed int)x - Map::Inst()->Width() / 2);
				if (tmp > std::abs((signed int)y - Map::Inst()->Height() / 2)) {
					currentJob().lock()->tasks[taskIndex] = Task(MOVE, Coordinate(x, 
						(y < (unsigned int)Map::Inst()->Height() / 2) ? 0 : Map::Inst()->Height()-1));
				} else {
					currentJob().lock()->tasks[taskIndex] = Task(MOVE, 
						Coordinate(((unsigned int)Map::Inst()->Width() / 2) ? 0 : Map::Inst()->Width()-1, 
						y));
				}
				currentJob().lock()->tasks.push_back(Task(FLEEMAP));
				break;

			case SLEEP:
				AddEffect(SLEEPING);
				AddEffect(BADSLEEP);
				weariness -= 50;
				if (weariness <= 0) {
					if (boost::shared_ptr<Entity> entity = currentEntity().lock()) {
						if (boost::static_pointer_cast<Construction>(entity)->HasTag(BED)) {
							RemoveEffect(BADSLEEP);
						}
					}
					TaskFinished(TASKSUCCESS);
					break;
				}
				break;

			case DISMANTLE:
				if (boost::shared_ptr<Construction> construct = boost::static_pointer_cast<Construction>(currentEntity().lock())) {
					construct->Condition(construct->Condition()-10);
					if (construct->Condition() <= 0) {
						Game::Inst()->RemoveConstruction(construct);
						TaskFinished(TASKSUCCESS);
						break;
					}
				}
				break;

			case WIELD:
				if (carried.lock()) {
					if (mainHand.lock()) { //Drop currently wielded weapon if such exists
						DropItem(mainHand);
						mainHand.reset();
					}
					mainHand = carried;
					carried.reset();
					TaskFinished(TASKSUCCESS);
#ifdef DEBUG
					std::cout<<name<<" wielded "<<mainHand.lock()->Name()<<"\n";
#endif
					break;
				}
				TaskFinished(TASKFAILFATAL, "(WIELD)Not carrying an item");
				break;

			case WEAR:
				if (carried.lock()) {
					if (carried.lock()->IsCategory(Item::StringToItemCategory("Armor"))) {
						if (armor.lock()) { //Remove armor and drop if already wearing
							DropItem(armor);
							armor.reset();
						}
						armor = carried;
#ifdef DEBUG
					std::cout<<name<<" wearing "<<armor.lock()->Name()<<"\n";
#endif
					}  else if (carried.lock()->IsCategory(Item::StringToItemCategory("Quiver"))) {
						if (quiver.lock()) { //Remove quiver and drop if already wearing
							DropItem(quiver);
							quiver.reset();
						}
						quiver = boost::static_pointer_cast<Container>(carried.lock());
#ifdef DEBUG
					std::cout<<name<<" wearing "<<quiver.lock()->Name()<<"\n";
#endif
					}
					carried.reset();
					TaskFinished(TASKSUCCESS);
					break;
				}
				TaskFinished(TASKFAILFATAL, "(WEAR)Not carrying an item");
				break;

			case BOGIRON:
				if (Map::Inst()->Type(x, y) == TILEBOG) {
					if (Random::Generate(UPDATES_PER_SECOND * 15 - 1) == 0) {
						bool stockpile = false;
						if (nextTask() && nextTask()->action == STOCKPILEITEM) stockpile = true;

						if (stockpile) {
							int item = Game::Inst()->CreateItem(Position(), Item::StringToItemType("Bog iron"), false);
							DropItem(carried);
							PickupItem(Game::Inst()->GetItem(item));
							stockpile = false;
						} else {
							Game::Inst()->CreateItem(Position(), Item::StringToItemType("Bog iron"), true);
						}
						TaskFinished(TASKSUCCESS);
						break;
					} else {
						break;
					}
				}
				TaskFinished(TASKFAILFATAL, "(BOGIRON)Not on a bog tile");
				break;

			case STOCKPILEITEM:
				if (carried.lock()) {
					boost::shared_ptr<Job> stockJob = Game::Inst()->StockpileItem(carried, true, true, false);
					if (stockJob) {
						stockJob->internal = true;
						//Add remaining tasks into stockjob
						for (unsigned int i = 1; taskIndex+i < jobs.front()->tasks.size(); ++i) {
							stockJob->tasks.push_back(jobs.front()->tasks[taskIndex+i]);
						}
						jobs.front()->tasks.clear();
						jobs.push_back(stockJob);
						DropItem(carried); //The stockpiling job will pickup the item
						carried.reset();
						TaskFinished(TASKSUCCESS);
						break;
					}
				}
				TaskFinished(TASKFAILFATAL, "(STOCKPILEITEM)Not carrying an item");
				break;

			case QUIVER:
				if (carried.lock()) {
					if (!quiver.lock()) {
						DropItem(carried);
						carried.reset();
						TaskFinished(TASKFAILFATAL, "(QUIVER)No quiver");
						break;
					}
					inventory->RemoveItem(carried);
					if (!quiver.lock()->AddItem(carried)) {
						DropItem(carried);
						carried.reset();
						TaskFinished(TASKFAILFATAL, "(QUIVER)Quiver full");
						break;
					}
					carried.reset();
					TaskFinished(TASKSUCCESS);
					break;
				}
				TaskFinished(TASKFAILFATAL, "(QUIVER)Not carrying an item");
				break;

			case FILL:
				if (carried.lock() && carried.lock()->IsCategory(Item::StringToItemCategory("Barrel"))) {
					boost::shared_ptr<Container> cont(boost::static_pointer_cast<Container>(carried.lock()));
					
					if (!cont->empty() && cont->ContainsWater() == 0 && cont->ContainsFilth() == 0) {
						//Not empty, but doesn't have water/filth, so it has items in it
						TaskFinished(TASKFAILFATAL, "(FILL)Attempting to fill non-empty container");
						break;
					}
					
					boost::weak_ptr<WaterNode> wnode = Map::Inst()->GetWater(currentTarget().X(), 
						currentTarget().Y());
					if (wnode.lock() && wnode.lock()->Depth() > 0 && cont->ContainsFilth() == 0) {
						int waterAmount = std::min(10, wnode.lock()->Depth());
						wnode.lock()->Depth(wnode.lock()->Depth()-waterAmount);
						cont->AddWater(waterAmount);
						TaskFinished(TASKSUCCESS);
						break;
					}

					boost::weak_ptr<FilthNode> fnode = Map::Inst()->GetFilth(currentTarget().X(),
						currentTarget().Y());
					if (fnode.lock() && fnode.lock()->Depth() > 0 && cont->ContainsWater() == 0) {
						int filthAmount = std::min(3, fnode.lock()->Depth());
						fnode.lock()->Depth(fnode.lock()->Depth()-filthAmount);
						cont->AddFilth(filthAmount);
						TaskFinished(TASKSUCCESS);
						break;
					}
					TaskFinished(TASKFAILFATAL, "(FILL)Nothing to fill container with");
					break;
				} 

				TaskFinished(TASKFAILFATAL, "(FILL)Not carrying a liquid container");
				break;

			case POUR:
				if (!carried.lock() || !boost::dynamic_pointer_cast<Container>(carried.lock())) {
					TaskFinished(TASKFAILFATAL, "(POUR)Not carrying a liquid container");
					break;
				}
				{
					boost::shared_ptr<Container> sourceContainer(boost::static_pointer_cast<Container>(carried.lock()));

					if (currentEntity().lock() && boost::dynamic_pointer_cast<Container>(currentEntity().lock())) {
						boost::shared_ptr<Container> targetContainer(boost::static_pointer_cast<Container>(currentEntity().lock()));
						if (sourceContainer->ContainsWater() > 0) {
							targetContainer->AddWater(sourceContainer->ContainsWater());
							sourceContainer->RemoveWater(sourceContainer->ContainsWater());
						} else {
							targetContainer->AddFilth(sourceContainer->ContainsFilth());
							sourceContainer->RemoveFilth(sourceContainer->ContainsFilth());
						}
						TaskFinished(TASKSUCCESS);
						break;
					} else if (currentTarget().X() >= 0 && currentTarget().Y() >= 0 && 
						currentTarget().X() < Map::Inst()->Width() && currentTarget().Y() < Map::Inst()->Height()) {
							if (sourceContainer->ContainsWater() > 0) {
								Game::Inst()->CreateWater(currentTarget(), sourceContainer->ContainsWater());
								sourceContainer->RemoveWater(sourceContainer->ContainsWater());
							} else {
								Game::Inst()->CreateFilth(currentTarget(), sourceContainer->ContainsFilth());
								sourceContainer->RemoveFilth(sourceContainer->ContainsFilth());
							}
							TaskFinished(TASKSUCCESS);
							break;
					}
				}
				TaskFinished(TASKFAILFATAL, "(POUR)No valid target");
				break;

			case DIG:
				if (!taskBegun) {
					timer = 0;
					taskBegun = true;
				} else {
					if (++timer >= 50) {
						Map::Inst()->Low(currentTarget().X(), currentTarget().Y(), true);
						Map::Inst()->Type(currentTarget().X(), currentTarget().Y(), TILEDITCH);
						TaskFinished(TASKSUCCESS);
					}
				}
				break;

			case FORGET:
				foundItem.reset();
				TaskFinished(TASKSUCCESS);
				break;

			case UNWIELD:
				if (mainHand.lock()) {
					foundItem = mainHand;
					DropItem(mainHand);
					mainHand.reset();
				}
				TaskFinished(TASKSUCCESS);
				break;

			case GETANGRY:
				aggressive = true;
				TaskFinished(TASKSUCCESS);
				break;

			case CALMDOWN:
				aggressive = false;
				TaskFinished(TASKSUCCESS);
				break;

			default: TaskFinished(TASKFAILFATAL, "*BUG*Unknown task*BUG*"); break;
			}
		} else {
			if (HasEffect(PANIC)) {
				JobManager::Inst()->NPCNotWaiting(uid);
				bool enemyFound = false;
				if (jobs.empty() && !nearNpcs.empty()) {
					boost::shared_ptr<Job> fleeJob(new Job("Flee"));
					fleeJob->internal = true;
					run = true;
					for (std::list<boost::weak_ptr<NPC> >::iterator npci = nearNpcs.begin(); npci != nearNpcs.end(); ++npci) {
						if (npci->lock() && (npci->lock()->faction != faction || npci->lock() == aggressor.lock())) {
							int dx = x - npci->lock()->x;
							int dy = y - npci->lock()->y;
							if (Map::Inst()->Walkable(x + dx, y + dy, (void *)this)) {
								fleeJob->tasks.push_back(Task(MOVE, Coordinate(x+dx,y+dy)));
								jobs.push_back(fleeJob);
							} else if (Map::Inst()->Walkable(x + dx, y, (void *)this)) {
								fleeJob->tasks.push_back(Task(MOVE, Coordinate(x+dx,y)));
								jobs.push_back(fleeJob);
							} else if (Map::Inst()->Walkable(x, y + dy, (void *)this)) {
								fleeJob->tasks.push_back(Task(MOVE, Coordinate(x,y+dy)));
								jobs.push_back(fleeJob);
							}
							enemyFound = true;
							break;
						}
					}
				}
			} else if (!GetSquadJob(boost::static_pointer_cast<NPC>(shared_from_this())) && 
				!FindJob(boost::static_pointer_cast<NPC>(shared_from_this()))) {
				boost::shared_ptr<Job> idleJob(new Job("Idle"));
				idleJob->internal = true;
				idleJob->tasks.push_back(Task(MOVENEAR, faction == 0 ? Camp::Inst()->Center() : Position()));
				idleJob->tasks.push_back(Task(WAIT, Coordinate(Random::Generate(9), 0)));
				jobs.push_back(idleJob);
				if (Distance(Camp::Inst()->Center().X(), Camp::Inst()->Center().Y(), x, y) < 15) run = false;
				else run = true;
			}
		}
	}

	return AINOTHING;
}

void NPC::StartJob(boost::shared_ptr<Job> job) {
	TaskFinished(TASKOWNDONE, "");

	if (job->RequiresTool() && (!mainHand.lock() || !mainHand.lock()->IsCategory(job->GetRequiredTool()))) {
		//We insert each one into the beginning, so these are inserted in reverse order
		job->tasks.insert(job->tasks.begin(), Task(FORGET)); /*"forget" the item we found, otherwise later tasks might
															 incorrectly refer to it */
		job->tasks.insert(job->tasks.begin(), Task(WIELD));
		job->tasks.insert(job->tasks.begin(), Task(TAKE));
		job->tasks.insert(job->tasks.begin(), Task(MOVE));
		job->tasks.insert(job->tasks.begin(), Task(FIND, Position(), boost::weak_ptr<Entity>(), job->GetRequiredTool()));
		addedTasksToCurrentJob = 5;
	}

	jobs.push_back(job);
	run = true;
}

TaskResult NPC::Move(TaskResult oldResult) {
	int moveX,moveY;
	if (run)
		nextMove += effectiveStats[MOVESPEED];
	else {
		if (effectiveStats[MOVESPEED]/3 == 0 && effectiveStats[MOVESPEED] != 0) ++nextMove;
		else nextMove += effectiveStats[MOVESPEED]/3;
	}
	while (nextMove > 100) {
		nextMove -= 100;
		boost::mutex::scoped_try_lock pathLock(pathMutex);
		if (pathLock.owns_lock()) {
			if (nopath) {nopath = false; return TASKFAILFATAL;}
			if (pathIndex < path->size() && pathIndex >= 0) {
				path->get(pathIndex, &moveX, &moveY); //Get next move

				if (pathIndex != path->size()-1 && Map::Inst()->NPCList(moveX, moveY)->size() > 0) {
					//Our next move target has an npc on it, and it isn't our target
					int nextX, nextY;
					path->get(pathIndex+1, &nextX, &nextY);
					/*Find a new target that is adjacent to our current, next, and the next after targets
					Effectively this makes the npc try and move around another npc, instead of walking onto
					the same tile and slowing down*/
					Map::Inst()->FindEquivalentMoveTarget(x, y, moveX, moveY, nextX, nextY, (void*)this);
				}

				if (Map::Inst()->Walkable(moveX, moveY, (void*)this)) { //If the tile is walkable, move there
					Position(Coordinate(moveX,moveY));
					Map::Inst()->WalkOver(moveX, moveY);
					++pathIndex;
				} else { //Encountered unexpected obstacle, fail and possibly repath
					return TASKFAILNONFATAL;
				}
				return TASKCONTINUE; //Everything is ok
			} else if (!findPathWorking) return PATHEMPTY; //No path
		}
	}
	//Can't move yet, so the earlier result is still valid
	return oldResult;
}

unsigned int NPC::pathingThreadCount = 0;
boost::mutex NPC::threadCountMutex;
void NPC::findPath(Coordinate target) {
	findPathWorking = true;
	pathIndex = 0;
	
	delete path;
	path = new TCODPath(Map::Inst()->Width(), Map::Inst()->Height(), Map::Inst(), (void*)this);

	if (pathingThreadCount < 12) {
		threadCountMutex.lock();
		++pathingThreadCount;
		threadCountMutex.unlock();
		boost::thread pathThread(boost::bind(tFindPath, path, x, y, target.X(), target.Y(), &pathMutex, &nopath, &findPathWorking, true));
	} else {
		tFindPath(path, x, y, target.X(), target.Y(), &pathMutex, &nopath, &findPathWorking, false);
	}
}

void NPC::speed(unsigned int value) {baseStats[MOVESPEED]=value;}
unsigned int NPC::speed() {return effectiveStats[MOVESPEED];}

void NPC::Draw(Coordinate upleft, TCODConsole *console) {
	int screenx = x - upleft.X();
	int screeny = y - upleft.Y();
	if (screenx >= 0 && screenx < console->getWidth() && screeny >= 0 && screeny < console->getHeight()) {
		if (statusGraphicCounter < 5 || statusEffectIterator == statusEffects.end()) {
			console->putCharEx(screenx, screeny, _graphic, _color, _bgcolor);
		} else {
			console->putCharEx(screenx, screeny, statusEffectIterator->graphic, statusEffectIterator->color, _bgcolor);
		}
	}
}

void NPC::GetTooltip(int x, int y, Tooltip *tooltip) {
	Entity::GetTooltip(x, y, tooltip);
	if(faction == 0 && !jobs.empty()) {
		boost::shared_ptr<Job> job = jobs.front();
		if(job->name != "Idle") {
			tooltip->AddEntry(TooltipEntry((boost::format("  %s") % job->name).str(), TCODColor::grey));
		}
	}
}

void NPC::color(TCODColor value, TCODColor bvalue) { _color = value; _bgcolor = bvalue; }
void NPC::graphic(int value) { _graphic = value; }

bool NPC::Expert() {return expert;}
void NPC::Expert(bool value) {expert = value;}

Coordinate NPC::Position() {return Coordinate(x,y);}

bool NPC::Dead() { return dead; }
void NPC::Kill() {
	if (!dead) {//You can't be killed if you're already dead!
		dead = true;
		health = 0;
		if (NPC::Presets[type].deathItem >= 0) {
			int corpsenum = Game::Inst()->CreateItem(Position(), NPC::Presets[type].deathItem, false);
			boost::shared_ptr<Item> corpse = Game::Inst()->GetItem(corpsenum).lock();
			corpse->Color(_color);
			corpse->Name(corpse->Name() + "(" + name + ")");
			if (velocity > 0) {
				corpse->CalculateFlightPath(GetVelocityTarget(), velocity, GetHeight());
			}
		} else if (NPC::Presets[type].deathItem == -1) {
			Game::Inst()->CreateFilth(Position());
		}

		while (!jobs.empty()) TaskFinished(TASKFAILFATAL, std::string("dead"));
		if (boost::shared_ptr<Item> weapon = mainHand.lock()) {
			weapon->Position(Position());
			weapon->PutInContainer();
			mainHand.reset();
		}

		if (boost::iequals(NPC::NPCTypeToString(type), "orc")) Announce::Inst()->AddMsg("An orc has died!", TCODColor::red, Position());
		else if (boost::iequals(NPC::NPCTypeToString(type), "goblin")) Announce::Inst()->AddMsg("A goblin has died!", TCODColor::red, Position());
	}
}

void NPC::DropItem(boost::weak_ptr<Item> item) {
	if (item.lock()) {
		inventory->RemoveItem(item);
		item.lock()->Position(Position());
		item.lock()->PutInContainer(boost::weak_ptr<Item>());
		bulk -= item.lock()->GetBulk();

		//If the item is a container with water/filth in it, spill it on the ground
		if (boost::dynamic_pointer_cast<Container>(item.lock())) {
			boost::shared_ptr<Container> cont(boost::static_pointer_cast<Container>(item.lock()));
			if (cont->ContainsWater() > 0) {
				Game::Inst()->CreateWater(Position(), cont->ContainsWater());
				cont->RemoveWater(cont->ContainsWater());
			} else if (cont->ContainsFilth() > 0) {
				Game::Inst()->CreateFilth(Position(), cont->ContainsFilth());
				cont->RemoveFilth(cont->ContainsFilth());
			}
		}
	}
}

Coordinate NPC::currentTarget() {
	if (currentTask()->target == Coordinate(-1,-1) && foundItem.lock()) {
		return foundItem.lock()->Position();
	}
	return currentTask()->target;
}

boost::weak_ptr<Entity> NPC::currentEntity() {
	if (currentTask()->entity.lock()) return currentTask()->entity;
	else if (foundItem.lock()) return boost::weak_ptr<Entity>(foundItem.lock());
	return boost::weak_ptr<Entity>();
}


void tFindPath(TCODPath *path, int x0, int y0, int x1, int y1, boost::try_mutex *pathMutex, bool *nopath, bool *findPathWorking, bool threaded) {
	boost::mutex::scoped_lock pathLock(*pathMutex);
	*nopath = !path->compute(x0, y0, x1, y1);
	*findPathWorking = false;
	if (threaded) {
		NPC::threadCountMutex.lock();
		--NPC::pathingThreadCount;
		NPC::threadCountMutex.unlock();
	}
}

bool NPC::GetSquadJob(boost::shared_ptr<NPC> npc) {
	if (boost::shared_ptr<Squad> squad = npc->MemberOf().lock()) {
		JobManager::Inst()->NPCNotWaiting(npc->uid);
		npc->aggressive = true;
		boost::shared_ptr<Job> newJob(new Job("Follow orders"));
		newJob->internal = true;

		//Priority #1, if the creature can wield a weapon get one if possible
		/*TODO: Right now this only makes friendlies take a weapon from a stockpile
		It should be expanded to allow all npc's to search for nearby weapons lying around. */
		if (!npc->mainHand.lock() && npc->GetFaction() == 0 && squad->Weapon() >= 0) {
			for (std::list<Attack>::iterator attacki = npc->attacks.begin(); attacki != npc->attacks.end();
				++attacki) {
					if (attacki->Type() == DAMAGE_WIELDED) {
						if (Game::Inst()->FindItemByCategoryFromStockpiles(
							squad->Weapon(), npc->Position()).lock()) {
								newJob->tasks.push_back(Task(FIND, npc->Position(), boost::shared_ptr<Entity>(), 
									squad->Weapon()));
								newJob->tasks.push_back(Task(MOVE));
								newJob->tasks.push_back(Task(TAKE));
								newJob->tasks.push_back(Task(WIELD));
								npc->jobs.push_back(newJob);
								npc->run = true;
								return true;
						}
						break;
					}
			}
		}

		if (npc->WieldingRangedWeapon()) {
			if (!npc->quiver.lock()) {
				if (Game::Inst()->FindItemByCategoryFromStockpiles(Item::StringToItemCategory("Quiver"), npc->Position()).lock()) {
						newJob->tasks.push_back(Task(FIND, npc->Position(), boost::shared_ptr<Entity>(), 
							Item::StringToItemCategory("Quiver")));
						newJob->tasks.push_back(Task(MOVE));
						newJob->tasks.push_back(Task(TAKE));
						newJob->tasks.push_back(Task(WEAR));
						npc->jobs.push_back(newJob);
						npc->run = true;
						return true;
				}
			} else if (npc->quiver.lock()->empty()) {
				if (Game::Inst()->FindItemByCategoryFromStockpiles(
					npc->mainHand.lock()->GetAttack().Projectile(), npc->Position()).lock()) {
						for (int i = 0; i < 10; ++i) {
							newJob->tasks.push_back(Task(FIND, npc->Position(), boost::shared_ptr<Entity>(), 
								npc->mainHand.lock()->GetAttack().Projectile()));
							newJob->tasks.push_back(Task(MOVE));
							newJob->tasks.push_back(Task(TAKE));
							newJob->tasks.push_back(Task(QUIVER));
						}
						npc->jobs.push_back(newJob);
						npc->run = true;
						return true;
				}
			}
		}

		if (!npc->armor.lock() && npc->GetFaction() == 0 && squad->Armor() >= 0) {
			npc->FindNewArmor();
		}

		switch (squad->GetOrder(npc->orderIndex)) { //GetOrder handles incrementing orderIndex
		case GUARD:
			if (squad->TargetCoordinate(npc->orderIndex).X() >= 0) {
				newJob->tasks.push_back(Task(MOVENEAR, squad->TargetCoordinate(npc->orderIndex)));
				//WAIT waits Coordinate.x / 5 seconds
				newJob->tasks.push_back(Task(WAIT, Coordinate(5*5, 0)));
				npc->jobs.push_back(newJob);
				if (Distance(npc->Position(), squad->TargetCoordinate(npc->orderIndex)) < 10) npc->run = false;
				else npc->run = true;
				return true;
			}
			break;

		case FOLLOW:
			if (squad->TargetEntity(npc->orderIndex).lock()) {
				newJob->tasks.push_back(Task(MOVENEAR, squad->TargetEntity(npc->orderIndex).lock()->Position(), squad->TargetEntity(npc->orderIndex)));
				npc->jobs.push_back(newJob);
				npc->run = true;
				return true;
			}
			break;

		default:
			break;
		}
	} 
	return false;
}

bool NPC::JobManagerFinder(boost::shared_ptr<NPC> npc) {
	if (!npc->MemberOf().lock()) {
		JobManager::Inst()->NPCWaiting(npc->uid);
	}
	return false;
}

void NPC::PlayerNPCReact(boost::shared_ptr<NPC> npc) {
	if (npc->aggressive) {
		if (npc->jobs.empty() || npc->currentTask()->action != KILL) {
			Game::Inst()->FindNearbyNPCs(npc, true);
			for (std::list<boost::weak_ptr<NPC> >::iterator npci = npc->nearNpcs.begin(); npci != npc->nearNpcs.end(); ++npci) {
				if (npci->lock()->faction != npc->faction) {
					JobManager::Inst()->NPCNotWaiting(npc->uid);
					boost::shared_ptr<Job> killJob(new Job("Kill "+npci->lock()->name));
					killJob->internal = true;
					killJob->tasks.push_back(Task(KILL, npci->lock()->Position(), *npci));
					while (!npc->jobs.empty()) npc->TaskFinished(TASKFAILNONFATAL);
#ifdef DEBUG
					std::cout<<"Push_back(killJob)\n";
#endif
					npc->jobs.push_back(killJob);
					npc->run = true;
				}
			}
		}
	} else if (npc->coward) { //Aggressiveness trumps cowardice
		Game::Inst()->FindNearbyNPCs(npc);
		for (std::list<boost::weak_ptr<NPC> >::iterator npci = npc->nearNpcs.begin(); npci != npc->nearNpcs.end(); ++npci) {
			if ((npci->lock()->GetFaction() != npc->faction || npci->lock() == npc->aggressor.lock()) && npci->lock()->aggressive) {
				JobManager::Inst()->NPCNotWaiting(npc->uid);
				while (!npc->jobs.empty()) npc->TaskFinished(TASKFAILNONFATAL);
				npc->AddEffect(PANIC);
			}
		}
	}
}

void NPC::PeacefulAnimalReact(boost::shared_ptr<NPC> animal) {
	Game::Inst()->FindNearbyNPCs(animal);
	for (std::list<boost::weak_ptr<NPC> >::iterator npci = animal->nearNpcs.begin(); npci != animal->nearNpcs.end(); ++npci) {
		if (npci->lock()->faction != animal->faction) {
			animal->AddEffect(PANIC);
		}
	}

	if (animal->aggressor.lock() && NPC::Presets[animal->type].tags.find("angers") != NPC::Presets[animal->type].tags.end()) {
		//Turn into a hostile animal if attacked by the player's creatures
		if (animal->aggressor.lock()->GetFaction() == 0){
			animal->FindJob = boost::bind(NPC::HostileAnimalFindJob, _1);
			animal->React = boost::bind(NPC::HostileAnimalReact, _1);
		}
		animal->aggressive = true;
		animal->RemoveEffect(PANIC);
		animal->AddEffect(RAGE);
	}
}

bool NPC::PeacefulAnimalFindJob(boost::shared_ptr<NPC> animal) {
	animal->aggressive = false;
	return false;
}

void NPC::HostileAnimalReact(boost::shared_ptr<NPC> animal) {
	animal->aggressive = true;
	Game::Inst()->FindNearbyNPCs(animal);
	for (std::list<boost::weak_ptr<NPC> >::iterator npci = animal->nearNpcs.begin(); npci != animal->nearNpcs.end(); ++npci) {
		if (npci->lock()->faction != animal->faction) {
			boost::shared_ptr<Job> killJob(new Job("Kill "+npci->lock()->name));
			killJob->internal = true;
			killJob->tasks.push_back(Task(KILL, npci->lock()->Position(), *npci));
			while (!animal->jobs.empty()) animal->TaskFinished(TASKFAILNONFATAL);
			animal->jobs.push_back(killJob);
			animal->run = true;
		}
	}
}

bool NPC::HostileAnimalFindJob(boost::shared_ptr<NPC> animal) {
	//For now hostile animals will simply attempt to get inside the settlement
	if (animal->Position() != Camp::Inst()->Center()) {
		boost::shared_ptr<Job> attackJob(new Job("Attack settlement"));
		attackJob->internal = true;
		attackJob->tasks.push_back(Task(MOVENEAR, Camp::Inst()->Center()));
		animal->jobs.push_back(attackJob);
		return true;
	}
	return false;
}

//A hungry animal will attempt to find food in the player's stockpiles and eat it,
//alternatively it will change into a "normal" hostile animal if no food is available
bool NPC::HungryAnimalFindJob(boost::shared_ptr<NPC> animal) {
	//We could use Task(FIND for this, but it doesn't give us feedback if there's
	//any food available
	boost::weak_ptr<Item> wfood = Game::Inst()->FindItemByCategoryFromStockpiles(Item::StringToItemCategory("Food"), animal->Position());
	if (boost::shared_ptr<Item> food = wfood.lock()) {
		//Found a food item
		boost::shared_ptr<Job> stealJob(new Job("Steal food"));
		stealJob->internal = true;
		stealJob->tasks.push_back(Task(MOVE, food->Position()));
		stealJob->tasks.push_back(Task(TAKE, food->Position(), food));
		stealJob->tasks.push_back(Task(FLEEMAP));
		animal->jobs.push_back(stealJob);
		return true;
	} else {
		animal->FindJob = boost::bind(NPC::HostileAnimalFindJob, _1);
	}
	return false;
}

void NPC::AddEffect(StatusEffectType effect) {
	for (std::list<StatusEffect>::iterator statusEffectI = statusEffects.begin(); statusEffectI != statusEffects.end(); ++statusEffectI) {
		if (statusEffectI->type == effect) {
			statusEffectI->cooldown = statusEffectI->cooldownDefault;
			return;
		}
	}

	statusEffects.push_back(StatusEffect(effect));
}

void NPC::RemoveEffect(StatusEffectType effect) {
	for (std::list<StatusEffect>::iterator statusEffectI = statusEffects.begin(); statusEffectI != statusEffects.end(); ++statusEffectI) {
		if (statusEffectI->type == effect) {
			if (statusEffectIterator == statusEffectI) ++statusEffectIterator;
			statusEffects.erase(statusEffectI);
			if (statusEffectIterator == statusEffects.end()) statusEffectIterator = statusEffects.begin();
			return;
		}
	}
}

bool NPC::HasEffect(StatusEffectType effect) {
	for (std::list<StatusEffect>::iterator statusEffectI = statusEffects.begin(); statusEffectI != statusEffects.end(); ++statusEffectI) {
		if (statusEffectI->type == effect) {
			return true;
		}
	}
	return false;
}

std::list<StatusEffect>* NPC::StatusEffects() { return &statusEffects; }

/*TODO: Calling jobs.clear() isn't a good idea as the NPC can have more than one job queued up, should use
TaskFinished(TASKFAILFATAL) or just remove the job we want aborted*/
void NPC::AbortCurrentJob(bool remove_job) {
	boost::shared_ptr<Job> job = jobs.front();
	TaskFinished(TASKFAILFATAL, "Job aborted");
	if (remove_job) { JobManager::Inst()->RemoveJob(job); }
}

void NPC::Hit(boost::weak_ptr<Entity> target, bool careful) {
	if (target.lock()) {
		if (boost::dynamic_pointer_cast<NPC>(target.lock())) {
			boost::shared_ptr<NPC> npc(boost::static_pointer_cast<NPC>(target.lock()));

			for (std::list<Attack>::iterator attacki = attacks.begin(); attacki != attacks.end(); ++attacki) {
				if (attacki->Cooldown() <= 0) {
					attacki->ResetCooldown();
					//First check if the target dodges the attack
					if (Random::Generate(99) < npc->effectiveStats[DODGE]) {
#ifdef DEBUG
						std::cout<<npc->name<<"("<<npc->uid<<") dodged\n";
#endif
						continue;
					}

					Attack attack = *attacki;
#ifdef DEBUG
					std::cout<<"attack.addsub: "<<attack.Amount().addsub<<"\n";
#endif
					
					if (attack.Type() == DAMAGE_WIELDED) {
#ifdef DEBUG
						std::cout<<"Wielded attack\n";
#endif
						GetMainHandAttack(attack);
					}
#ifdef DEBUG
					std::cout<<"attack.addsub after: "<<attack.Amount().addsub<<"\n";
#endif
					if (!careful && effectiveStats[STRENGTH] >= npc->effectiveStats[SIZE]) {
						if (attack.Type() == DAMAGE_BLUNT || Random::GenerateBool()) {
							Coordinate tar;
							tar.X((npc->Position().X() - x) * std::max(effectiveStats[STRENGTH] - npc->effectiveStats[SIZE], 1));
							tar.Y((npc->Position().Y() - y) * std::max(effectiveStats[STRENGTH] - npc->effectiveStats[SIZE], 1));
							npc->CalculateFlightPath(npc->Position()+tar, Random::Generate(25, 19 + 25));
							npc->pathIndex = -1;
						}
					}

					npc->Damage(&attack, boost::static_pointer_cast<NPC>(shared_from_this()));
				}
			}
		}
	}
}

void NPC::FireProjectile(boost::weak_ptr<Entity> target) {
	for (std::list<Attack>::iterator attacki = attacks.begin(); attacki != attacks.end(); ++attacki) {
		if (attacki->Type() == DAMAGE_WIELDED) {
			if (attacki->Cooldown() <= 0) {
				attacki->ResetCooldown();

				if (target.lock() && !quiver.lock()->empty()) {
					boost::shared_ptr<Item> projectile = quiver.lock()->GetFirstItem().lock();
					quiver.lock()->RemoveItem(projectile);
					projectile->PutInContainer();
					projectile->Position(Position());
					projectile->CalculateFlightPath(target.lock()->Position(), 50, GetHeight());
				}
			}
			break;
		}
	}
 }

void NPC::Damage(Attack* attack, boost::weak_ptr<NPC> aggr) {
	Resistance res;

	switch (attack->Type()) {
	case DAMAGE_SLASH:
	case DAMAGE_PIERCE:
	case DAMAGE_BLUNT: res = PHYSICAL_RES; break;

	case DAMAGE_MAGIC: res = MAGIC_RES; break;

	case DAMAGE_FIRE: res = FIRE_RES; break;

	case DAMAGE_COLD: res = COLD_RES; break;

	case DAMAGE_POISON: res = POISON_RES; break;

	default: res = PHYSICAL_RES; break;
	}
						
	double resistance = (100.0 - (float)effectiveResistances[res]) / 100.0;
	int damage = (int)(Game::DiceToInt(attack->Amount()) * resistance);
	health -= damage;

	#ifdef DEBUG
	std::cout<<"Resistance: "<<resistance<<"\n";
	std::cout<<name<<"("<<uid<<") inflicted "<<damage<<" damage\n";
	#endif

	for (unsigned int effecti = 0; effecti < attack->StatusEffects()->size(); ++effecti) {
		if (Random::Generate(99) < attack->StatusEffects()->at(effecti).second) {
			AddEffect(attack->StatusEffects()->at(effecti).first);
		}
	}

	if (health <= 0) Kill();

	if (damage > 0) {
		Game::Inst()->CreateBlood(Coordinate(
			Position().X() + Random::Generate(-1, 1),
			Position().Y() + Random::Generate(-1, 1)
		));
		if (aggr.lock()) aggressor = aggr;
	}
}

void NPC::MemberOf(boost::weak_ptr<Squad> newSquad) {
	squad = newSquad;
	if (!squad.lock()) { //NPC was removed from a squad
		//Drop weapon, quiver and armor
		std::list<boost::shared_ptr<Item> > equipment;
		if (mainHand.lock()) {
			equipment.push_back(mainHand.lock());
			mainHand.reset();
		}
		if (armor.lock()) { 
			equipment.push_back(armor.lock());
			armor.reset();
		}
		if (quiver.lock()) {
			equipment.push_back(quiver.lock());
			quiver.reset();
		}

		for (std::list<boost::shared_ptr<Item> >::iterator eqit = equipment.begin(); eqit != equipment.end(); ++eqit) {
			inventory->RemoveItem(*eqit);
			(*eqit)->Position(Position());
			(*eqit)->PutInContainer();
		}

		aggressive = false;
	}
}
boost::weak_ptr<Squad> NPC::MemberOf() {return squad;}

void NPC::Escape() {
	if (carried.lock()) {
		Announce::Inst()->AddMsg((boost::format("%s has escaped with [%s]!") % name % carried.lock()->Name()).str(), 
			TCODColor::yellow, Position());
	}
	DestroyAllItems();
	escaped = true;
}

void NPC::DestroyAllItems() {
	for (std::set<boost::weak_ptr<Item> >::iterator it = inventory->begin(); it != inventory->end(); ++it) {
		if (it->lock()) Game::Inst()->RemoveItem(*it);
	}
}

bool NPC::Escaped() { return escaped; }

class NPCListener : public ITCODParserListener {
	bool parserNewStruct(TCODParser *parser,const TCODParserStruct *str,const char *name) {
#ifdef DEBUG
		std::cout<<boost::format("new %s structure: ") % str->getName();
#endif
		if (boost::iequals(str->getName(), "npc_type")) {
			NPC::Presets.push_back(NPCPreset(name));
			NPC::NPCTypeNames[name] = NPC::Presets.size()-1;
#ifdef DEBUG
			std::cout<<name<<"\n";
#endif
		} else if (boost::iequals(str->getName(), "attack")) {
			NPC::Presets.back().attacks.push_back(Attack());
#ifdef DEBUG
			std::cout<<name<<"\n";
#endif
		} else if (boost::iequals(str->getName(), "resistances")) {
#ifdef DEBUG
			std::cout<<"\n";
#endif
		}
		return true;
	}
	bool parserFlag(TCODParser *parser,const char *name) {
#ifdef DEBUG
		std::cout<<(boost::format("%s\n") % name).str();
#endif
		if (boost::iequals(name,"generateName")) { NPC::Presets.back().generateName = true; }
		else if (boost::iequals(name,"needsNutrition")) { NPC::Presets.back().needsNutrition = true; }
		else if (boost::iequals(name,"needsSleep")) { NPC::Presets.back().needsSleep = true; }
		else if (boost::iequals(name,"expert")) { NPC::Presets.back().expert = true; }
		return true;
	}
	bool parserProperty(TCODParser *parser,const char *name, TCOD_value_type_t type, TCOD_value_t value) {
#ifdef DEBUG
		std::cout<<(boost::format("%s\n") % name).str();
#endif
		if (boost::iequals(name,"name")) { NPC::Presets.back().name = value.s; }
		else if (boost::iequals(name,"plural")) { NPC::Presets.back().plural = value.s; }
		else if (boost::iequals(name,"speed")) { NPC::Presets.back().stats[MOVESPEED] = value.i; }
		else if (boost::iequals(name,"color")) { NPC::Presets.back().color = value.col; }
		else if (boost::iequals(name,"graphic")) { NPC::Presets.back().graphic = value.c; }
		else if (boost::iequals(name,"health")) { NPC::Presets.back().health = value.i; }
		else if (boost::iequals(name,"AI")) { NPC::Presets.back().ai = value.s; }
		else if (boost::iequals(name,"dodge")) { NPC::Presets.back().stats[DODGE] = value.i; }
		else if (boost::iequals(name,"spawnAsGroup")) { 
			NPC::Presets.back().spawnAsGroup = true;
			NPC::Presets.back().group = value.dice;
		} else if (boost::iequals(name,"type")) {
			NPC::Presets.back().attacks.back().Type(Attack::StringToDamageType(value.s));
		} else if (boost::iequals(name,"damage")) {
			NPC::Presets.back().attacks.back().Amount(value.dice);
		} else if (boost::iequals(name,"cooldown")) {
			NPC::Presets.back().attacks.back().CooldownMax(value.i);
		} else if (boost::iequals(name,"statusEffects")) {
			for (int i = 0; i < TCOD_list_size(value.list); ++i) {
				NPC::Presets.back().attacks.back().StatusEffects()->push_back(std::pair<StatusEffectType, int>(StatusEffect::StringToStatusEffectType((char*)TCOD_list_get(value.list,i)), 100));
			}
		} else if (boost::iequals(name,"effectChances")) {
			for (int i = 0; i < TCOD_list_size(value.list); ++i) {
				NPC::Presets.back().attacks.back().StatusEffects()->at(i).second = (intptr_t)TCOD_list_get(value.list,i);
			}
		} else if (boost::iequals(name,"projectile")) {
			NPC::Presets.back().attacks.back().Projectile(Item::StringToItemType(value.s));
		} else if (boost::iequals(name,"physical")) {
			NPC::Presets.back().resistances[PHYSICAL_RES] = value.i;
		} else if (boost::iequals(name,"magic")) {
			NPC::Presets.back().resistances[MAGIC_RES] = value.i;
		} else if (boost::iequals(name,"cold")) {
			NPC::Presets.back().resistances[COLD_RES] = value.i;
		} else if (boost::iequals(name,"fire")) {
			NPC::Presets.back().resistances[FIRE_RES] = value.i;
		} else if (boost::iequals(name,"poison")) {
			NPC::Presets.back().resistances[POISON_RES] = value.i;
		} else if (boost::iequals(name,"tags")) {
			for (int i = 0; i < TCOD_list_size(value.list); ++i) {
				std::string tag = (char*)TCOD_list_get(value.list,i);
				NPC::Presets.back().tags.insert(boost::to_lower_copy(tag));
			}
		} else if (boost::iequals(name,"strength")) {
			NPC::Presets.back().stats[STRENGTH] = value.i;
		} else if (boost::iequals(name,"size")) {
			NPC::Presets.back().stats[SIZE] = value.i;
			if (NPC::Presets.back().stats[STRENGTH] == 1) NPC::Presets.back().stats[STRENGTH] = value.i;
		} else if (boost::iequals(name,"tier")) {
			NPC::Presets.back().tier = value.i;
		} else if (boost::iequals(name,"death")) {
			if (boost::iequals(value.s,"filth")) NPC::Presets.back().deathItem = -1;
			else NPC::Presets.back().deathItem = Item::StringToItemType(value.s);
		}
		return true;
	}
	bool parserEndStruct(TCODParser *parser,const TCODParserStruct *str,const char *name) {
#ifdef DEBUG
		std::cout<<boost::format("end of %s\n") % str->getName();
#endif
		if (NPC::Presets.back().plural == "") NPC::Presets.back().plural = NPC::Presets.back().name + "s";
		return true;
	}
	void error(const char *msg) {
		LOG("NPCListener: " << msg);
		Game::Inst()->Exit(false);
	}
};

void NPC::LoadPresets(std::string filename) {
	TCODParser parser = TCODParser();
	TCODParserStruct *npcTypeStruct = parser.newStructure("npc_type");
	npcTypeStruct->addProperty("name", TCOD_TYPE_STRING, true);
	npcTypeStruct->addProperty("plural", TCOD_TYPE_STRING, false);
	npcTypeStruct->addProperty("color", TCOD_TYPE_COLOR, true);
	npcTypeStruct->addProperty("graphic", TCOD_TYPE_CHAR, true);
	npcTypeStruct->addFlag("expert");
	const char* aiTypes[] = { "PlayerNPC", "PeacefulAnimal", "HungryAnimal", "HostileAnimal", NULL }; 
	npcTypeStruct->addValueList("AI", aiTypes, true);
	npcTypeStruct->addFlag("needsNutrition");
	npcTypeStruct->addFlag("needsSleep");
	npcTypeStruct->addFlag("generateName");
	npcTypeStruct->addProperty("spawnAsGroup", TCOD_TYPE_DICE, false);
	npcTypeStruct->addListProperty("tags", TCOD_TYPE_STRING, false);
	npcTypeStruct->addProperty("tier", TCOD_TYPE_INT, false);
	npcTypeStruct->addProperty("death", TCOD_TYPE_STRING, false);
	
	TCODParserStruct *attackTypeStruct = parser.newStructure("attack");
	const char* damageTypes[] = { "slashing", "piercing", "blunt", "magic", "fire", "cold", "poison", "wielded", NULL };
	attackTypeStruct->addValueList("type", damageTypes, true);
	attackTypeStruct->addProperty("damage", TCOD_TYPE_DICE, false);
	attackTypeStruct->addProperty("cooldown", TCOD_TYPE_INT, false);
	attackTypeStruct->addListProperty("statusEffects", TCOD_TYPE_STRING, false);
	attackTypeStruct->addListProperty("effectChances", TCOD_TYPE_INT, false);
	attackTypeStruct->addFlag("ranged");
	attackTypeStruct->addProperty("projectile", TCOD_TYPE_STRING, false);

	TCODParserStruct *resistancesStruct = parser.newStructure("resistances");
	resistancesStruct->addProperty("physical", TCOD_TYPE_INT, false);
	resistancesStruct->addProperty("magic", TCOD_TYPE_INT, false);
	resistancesStruct->addProperty("cold", TCOD_TYPE_INT, false);
	resistancesStruct->addProperty("fire", TCOD_TYPE_INT, false);
	resistancesStruct->addProperty("poison", TCOD_TYPE_INT, false);

	TCODParserStruct *statsStruct = parser.newStructure("stats");
	statsStruct->addProperty("health", TCOD_TYPE_INT, true);
	statsStruct->addProperty("speed", TCOD_TYPE_INT, true);
	statsStruct->addProperty("dodge", TCOD_TYPE_INT, true);
	statsStruct->addProperty("size", TCOD_TYPE_INT, true);
	statsStruct->addProperty("strength", TCOD_TYPE_INT, false);

	npcTypeStruct->addStructure(attackTypeStruct);
	npcTypeStruct->addStructure(resistancesStruct);
	npcTypeStruct->addStructure(statsStruct);

	parser.run(filename.c_str(), new NPCListener());
}

std::string NPC::NPCTypeToString(NPCType type) {
	return Presets[type].typeName;
}

NPCType NPC::StringToNPCType(std::string typeName) {
	return NPCTypeNames[typeName];
}

int NPC::GetNPCSymbol() { return Presets[type].graphic; }

void NPC::InitializeAIFunctions() {
	if (NPC::Presets[type].ai == "PlayerNPC") {
		FindJob = boost::bind(NPC::JobManagerFinder, _1);
		React = boost::bind(NPC::PlayerNPCReact, _1);
		faction = 0;
	} else if (NPC::Presets[type].ai == "PeacefulAnimal") {
		FindJob = boost::bind(NPC::PeacefulAnimalFindJob, _1);
		React = boost::bind(NPC::PeacefulAnimalReact, _1);
		faction = 1;
	} else if (NPC::Presets[type].ai == "HungryAnimal") {
		FindJob = boost::bind(NPC::HungryAnimalFindJob, _1);
		React = boost::bind(NPC::HostileAnimalReact, _1);
		faction = 2;
	} else if (NPC::Presets[type].ai == "HostileAnimal") {
		FindJob = boost::bind(NPC::HostileAnimalFindJob, _1);
		React = boost::bind(NPC::HostileAnimalReact, _1);
		faction = 2;
	}
}

void NPC::GetMainHandAttack(Attack &attack) {
	attack.Type(DAMAGE_BLUNT);
	if (boost::shared_ptr<Item> weapon = mainHand.lock()) {
		Attack wAttack = weapon->GetAttack();
		attack.Type(wAttack.Type());
		attack.AddDamage(wAttack.Amount());
		attack.Projectile(wAttack.Projectile());
		for (std::vector<std::pair<StatusEffectType, int> >::iterator effecti = wAttack.StatusEffects()->begin();
			effecti != wAttack.StatusEffects()->end(); ++effecti) {
				attack.StatusEffects()->push_back(*effecti);
		}
	}
}

bool NPC::WieldingRangedWeapon() {
	if (boost::shared_ptr<Item> weapon = mainHand.lock()) {
		Attack wAttack = weapon->GetAttack();
		return wAttack.Type() == DAMAGE_RANGED;
	}
	return false;
}

void NPC::FindNewWeapon() {
	int weaponValue = 0;
	if (mainHand.lock() && mainHand.lock()->IsCategory(squad.lock()->Weapon())) {
		weaponValue = mainHand.lock()->RelativeValue();
	}
	ItemCategory weaponCategory = squad.lock() ? squad.lock()->Weapon() : Item::StringToItemCategory("Weapon");
	boost::weak_ptr<Item> newWeapon = Game::Inst()->FindItemByCategoryFromStockpiles(weaponCategory, Position(), BETTERTHAN, weaponValue);
	if (boost::shared_ptr<Item> weapon = newWeapon.lock()) {
		boost::shared_ptr<Job> weaponJob(new Job("Grab weapon"));
		weaponJob->internal = true;
		weaponJob->ReserveEntity(weapon);
		weaponJob->tasks.push_back(Task(MOVE, weapon->Position()));
		weaponJob->tasks.push_back(Task(TAKE, weapon->Position(), weapon));
		weaponJob->tasks.push_back(Task(WIELD));
		jobs.push_back(weaponJob);
		run = true;
	}
}

void NPC::FindNewArmor() {
	int armorValue = 0;
	if (armor.lock() && armor.lock()->IsCategory(squad.lock()->Armor())) {
		armorValue = armor.lock()->RelativeValue();
	}
	ItemCategory armorCategory = squad.lock() ? squad.lock()->Armor() : Item::StringToItemCategory("Armor");
	boost::weak_ptr<Item> newArmor = Game::Inst()->FindItemByCategoryFromStockpiles(armorCategory, Position(), BETTERTHAN, armorValue);
	if (boost::shared_ptr<Item> arm = newArmor.lock()) {
		boost::shared_ptr<Job> armorJob(new Job("Grab armor"));
		armorJob->internal = true;
		armorJob->ReserveEntity(arm);
		armorJob->tasks.push_back(Task(MOVE, arm->Position()));
		armorJob->tasks.push_back(Task(TAKE, arm->Position(), arm));
		armorJob->tasks.push_back(Task(WEAR));
		jobs.push_back(armorJob);
		run = true;
	}
}

boost::weak_ptr<Item> NPC::Wielding() {
	return mainHand;
}

boost::weak_ptr<Item> NPC::Wearing() {
	return armor;
}

bool NPC::HasHands() { return hasHands; }

void NPC::UpdateVelocity() {
	if (velocity > 0) {
		nextVelocityMove += velocity;
		while (nextVelocityMove > 100) {
			nextVelocityMove -= 100;
			if (flightPath.size() > 0) {
				if (flightPath.back().height < ENTITYHEIGHT) { //We're flying low enough to hit things
					int tx = flightPath.back().coord.X();
					int ty = flightPath.back().coord.Y();
					if (Map::Inst()->BlocksWater(tx,ty)) { //We've hit an obstacle
						health -= velocity/5;
						AddEffect(CONCUSSION);

						if (Map::Inst()->GetConstruction(tx,ty) > -1) {
							if (boost::shared_ptr<Construction> construct = Game::Inst()->GetConstruction(Map::Inst()->GetConstruction(tx,ty)).lock()) {
								//TODO: Create attack based on weight and damage construct
							}
						}

						SetVelocity(0);
						flightPath.clear();
						return;
					}
					if (Map::Inst()->NPCList(tx,ty)->size() > 0 && Random::Generate(9) < (signed int)(2 + Map::Inst()->NPCList(tx,ty)->size())) {
						health -= velocity/5;
						AddEffect(CONCUSSION);
						SetVelocity(0);
						flightPath.clear();
						return;
					}
				}
				if (flightPath.back().height == 0) {
					health -= velocity/5;
					AddEffect(CONCUSSION);
					SetVelocity(0);
				}
				Position(flightPath.back().coord);
				
				flightPath.pop_back();
			} else SetVelocity(0);
		}
	} else { //We're not hurtling through air so let's tumble around if we're stuck on unwalkable terrain
		if (!Map::Inst()->Walkable(x, y, (void*)this)) {
			for (int radius = 1; radius < 10; ++radius) {
				for (unsigned int xi = x - radius; xi <= x + radius; ++xi) {
					for (unsigned int yi = y - radius; yi <= y + radius; ++yi) {
						if (Map::Inst()->Walkable(xi, yi, (void*)this)) {
							Position(Coordinate(xi, yi));
							return;
						}
					}
				}
			}
		}
	}
}

void NPC::PickupItem(boost::weak_ptr<Item> item) {
	if (item.lock()) {
		carried = boost::static_pointer_cast<Item>(item.lock());
		bulk += item.lock()->GetBulk();
		if (!inventory->AddItem(carried)) Announce::Inst()->AddMsg("No space in inventory");
	}
}

NPCPreset::NPCPreset(std::string typeNameVal) : 
	typeName(typeNameVal),
	name("AA Club"),
	plural(""),
	color(TCODColor::pink),
	graphic('?'),
	expert(false),
	health(10),
	ai("PeacefulAnimal"),
	needsNutrition(false),
	needsSleep(false),
	generateName(false),
	spawnAsGroup(false),
	group(TCOD_dice_t()),
	attacks(std::list<Attack>()),
	tags(std::set<std::string>()),
	tier(0),
	deathItem(-2)
{
	for (int i = 0; i < STAT_COUNT; ++i) {
		stats[i] = 1;
	}
	for (int i = 0; i < RES_COUNT; ++i) {
		resistances[i] = 0;
	}
	group.addsub = 0;
	group.multiplier = 1;
	group.nb_dices = 1;
	group.nb_faces = 1;
}

int NPC::GetHealth() { return health; }
int NPC::GetMaxHealth() { return maxHealth; }

void NPC::AbortJob(boost::weak_ptr<Job> wjob) {
	if (boost::shared_ptr<Job> job = wjob.lock()) {
		for (std::deque<boost::shared_ptr<Job> >::iterator jobi = jobs.begin(); jobi != jobs.end(); ++jobi) {
			if (*jobi == job) {
				if (job == jobs.front()) {
					TaskFinished(TASKFAILFATAL, "(AbortJob)");
				}
				return;
			}
		}
	}
}