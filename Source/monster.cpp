/**
 * @file monster.cpp
 *
 * Implementation of monster functionality, AI, actions, spawning, loading, etc.
 */
#include "monster.h"

#include <climits>

#include <algorithm>
#include <array>

#include <fmt/compile.h>
#include <fmt/format.h>

#include "control.h"
#include "cursor.h"
#include "dead.h"
#include "engine/load_cl2.hpp"
#include "engine/load_file.hpp"
#include "engine/points_in_rectangle_range.hpp"
#include "engine/random.hpp"
#include "engine/render/clx_render.hpp"
#include "engine/world_tile.hpp"
#include "init.h"
#include "levels/crypt.h"
#include "levels/drlg_l4.h"
#include "levels/themes.h"
#include "levels/trigs.h"
#include "lighting.h"
#include "minitext.h"
#include "missiles.h"
#include "movie.h"
#include "options.h"
#include "spelldat.h"
#include "storm/storm_net.hpp"
#include "towners.h"
#include "utils/cl2_to_clx.hpp"
#include "utils/file_name_generator.hpp"
#include "utils/language.h"
#include "utils/stdcompat/string_view.hpp"
#include "utils/str_cat.hpp"
#include "utils/utf8.hpp"

#ifdef _DEBUG
#include "debug.h"
#endif

namespace devilution {

CMonster LevelMonsterTypes[MaxLvlMTypes];
size_t LevelMonsterTypeCount;
Monster Monsters[MaxMonsters];
int ActiveMonsters[MaxMonsters];
size_t ActiveMonsterCount;
// BUGFIX: replace MonsterKillCounts[MaxMonsters] with MonsterKillCounts[NUM_MTYPES].
/** Tracks the total number of monsters killed per monster_id. */
int MonsterKillCounts[MaxMonsters];
bool sgbSaveSoundOn;

namespace {

constexpr int NightmareToHitBonus = 85;
constexpr int HellToHitBonus = 120;

constexpr int NightmareAcBonus = 50;
constexpr int HellAcBonus = 80;

/** Tracks which missile files are already loaded */
size_t totalmonsters;
int monstimgtot;
int uniquetrans;

constexpr const std::array<_monster_id, 12> SkeletonTypes {
	MT_WSKELAX,
	MT_TSKELAX,
	MT_RSKELAX,
	MT_XSKELAX,
	MT_WSKELBW,
	MT_TSKELBW,
	MT_RSKELBW,
	MT_XSKELBW,
	MT_WSKELSD,
	MT_TSKELSD,
	MT_RSKELSD,
	MT_XSKELSD,
};

/** Maps from monster action to monster animation letter. */
constexpr char Animletter[7] = "nwahds";

size_t GetNumAnims(const MonsterData &monsterData)
{
	return monsterData.hasSpecial ? 6 : 5;
}

void InitMonsterTRN(CMonster &monst)
{
	char path[64];
	*BufCopy(path, "monsters\\", monst.data->trnFile, ".trn") = '\0';
	std::array<uint8_t, 256> colorTranslations;
	LoadFileInMem(path, colorTranslations);
	std::replace(colorTranslations.begin(), colorTranslations.end(), 255, 0);

	const size_t numAnims = GetNumAnims(*monst.data);
	for (size_t i = 0; i < numAnims; i++) {
		if (i == 1 && IsAnyOf(monst.type, MT_COUNSLR, MT_MAGISTR, MT_CABALIST, MT_ADVOCATE)) {
			continue;
		}

		AnimStruct &anim = monst.anims[i];
		if (anim.sprites->isSheet()) {
			ClxApplyTrans(ClxSpriteSheet { anim.sprites->sheet() }, colorTranslations.data());
		} else {
			ClxApplyTrans(ClxSpriteList { anim.sprites->list() }, colorTranslations.data());
		}
	}
}

void InitMonster(Monster &monster, Direction rd, size_t typeIndex, Point position)
{
	monster.direction = rd;
	monster.position.tile = position;
	monster.position.future = position;
	monster.position.old = position;
	monster.levelType = typeIndex;
	monster.mode = MonsterMode::Stand;
	monster.animInfo = {};
	monster.changeAnimationData(MonsterGraphic::Stand);
	monster.animInfo.tickCounterOfCurrentFrame = GenerateRnd(monster.animInfo.ticksPerFrame - 1);
	monster.animInfo.currentFrame = GenerateRnd(monster.animInfo.numberOfFrames - 1);

	int maxhp = monster.data().hitPointsMinimum + GenerateRnd(monster.data().hitPointsMaximum - monster.data().hitPointsMinimum + 1);
	if (monster.type().type == MT_DIABLO && !gbIsHellfire) {
		maxhp /= 2;
	}
	monster.maxHitPoints = maxhp << 6;

	if (!gbIsMultiplayer)
		monster.maxHitPoints = std::max(monster.maxHitPoints / 2, 64);

	monster.hitPoints = monster.maxHitPoints;
	monster.ai = monster.data().ai;
	monster.intelligence = monster.data().intelligence;
	monster.goal = MonsterGoal::Normal;
	monster.goalVar1 = 0;
	monster.goalVar2 = 0;
	monster.goalVar3 = 0;
	monster.pathCount = 0;
	monster.isInvalid = false;
	monster.uniqueType = UniqueMonsterType::None;
	monster.activeForTicks = 0;
	monster.lightId = NO_LIGHT; // BUGFIX monsters initial light id should be -1 (fixed)
	monster.rndItemSeed = AdvanceRndSeed();
	monster.aiSeed = AdvanceRndSeed();
	monster.whoHit = 0;
	monster.toHit = monster.data().toHit;
	monster.minDamage = monster.data().minDamage;
	monster.maxDamage = monster.data().maxDamage;
	monster.minDamageSpecial = monster.data().minDamageSpecial;
	monster.maxDamageSpecial = monster.data().maxDamageSpecial;
	monster.armorClass = monster.data().armorClass;
	monster.resistance = monster.data().resistance;
	monster.leader = Monster::NoLeader;
	monster.leaderRelation = LeaderRelation::None;
	monster.flags = monster.data().abilityFlags;
	monster.talkMsg = TEXT_NONE;

	if (monster.ai == AI_GARG) {
		monster.changeAnimationData(MonsterGraphic::Special);
		monster.animInfo.currentFrame = 0;
		monster.flags |= MFLAG_ALLOW_SPECIAL;
		monster.mode = MonsterMode::SpecialMeleeAttack;
	}

	if (sgGameInitInfo.nDifficulty == DIFF_NIGHTMARE) {
		monster.maxHitPoints = 3 * monster.maxHitPoints;
		if (gbIsHellfire)
			monster.maxHitPoints += (gbIsMultiplayer ? 100 : 50) << 6;
		else
			monster.maxHitPoints += 64;
		monster.hitPoints = monster.maxHitPoints;
		monster.toHit += NightmareToHitBonus;
		monster.minDamage = 2 * (monster.minDamage + 2);
		monster.maxDamage = 2 * (monster.maxDamage + 2);
		monster.minDamageSpecial = 2 * (monster.minDamageSpecial + 2);
		monster.maxDamageSpecial = 2 * (monster.maxDamageSpecial + 2);
		monster.armorClass += NightmareAcBonus;
	} else if (sgGameInitInfo.nDifficulty == DIFF_HELL) {
		monster.maxHitPoints = 4 * monster.maxHitPoints;
		if (gbIsHellfire)
			monster.maxHitPoints += (gbIsMultiplayer ? 200 : 100) << 6;
		else
			monster.maxHitPoints += 192;
		monster.hitPoints = monster.maxHitPoints;
		monster.toHit += HellToHitBonus;
		monster.minDamage = 4 * monster.minDamage + 6;
		monster.maxDamage = 4 * monster.maxDamage + 6;
		monster.minDamageSpecial = 4 * monster.minDamageSpecial + 6;
		monster.maxDamageSpecial = 4 * monster.maxDamageSpecial + 6;
		monster.armorClass += HellAcBonus;
		monster.resistance = monster.data().resistanceHell;
	}
}

bool CanPlaceMonster(Point position)
{
	return InDungeonBounds(position)
	    && dMonster[position.x][position.y] == 0
	    && dPlayer[position.x][position.y] == 0
	    && !IsTileVisible(position)
	    && !TileContainsSetPiece(position)
	    && !IsTileOccupied(position);
}

void PlaceMonster(int i, size_t typeIndex, Point position)
{
	if (LevelMonsterTypes[typeIndex].type == MT_NAKRUL) {
		for (size_t j = 0; j < ActiveMonsterCount; j++) {
			if (Monsters[j].levelType == typeIndex) {
				return;
			}
		}
	}
	dMonster[position.x][position.y] = i + 1;

	auto rd = static_cast<Direction>(GenerateRnd(8));
	InitMonster(Monsters[i], rd, typeIndex, position);
}

void PlaceGroup(size_t typeIndex, unsigned num, Monster *leader = nullptr, bool leashed = false)
{
	unsigned placed = 0;

	for (int try1 = 0; try1 < 10; try1++) {
		while (placed != 0) {
			ActiveMonsterCount--;
			placed--;
			const auto &position = Monsters[ActiveMonsterCount].position.tile;
			dMonster[position.x][position.y] = 0;
		}

		int xp;
		int yp;
		if (leader != nullptr) {
			int offset = GenerateRnd(8);
			auto position = leader->position.tile + static_cast<Direction>(offset);
			xp = position.x;
			yp = position.y;
		} else {
			do {
				xp = GenerateRnd(80) + 16;
				yp = GenerateRnd(80) + 16;
			} while (!CanPlaceMonster({ xp, yp }));
		}
		int x1 = xp;
		int y1 = yp;

		if (num + ActiveMonsterCount > totalmonsters) {
			num = totalmonsters - ActiveMonsterCount;
		}

		unsigned j = 0;
		for (unsigned try2 = 0; j < num && try2 < 100; xp += Displacement(static_cast<Direction>(GenerateRnd(8))).deltaX, yp += Displacement(static_cast<Direction>(GenerateRnd(8))).deltaX) { /// BUGFIX: `yp += Point.y`
			if (!CanPlaceMonster({ xp, yp })
			    || (dTransVal[xp][yp] != dTransVal[x1][y1])
			    || (leashed && (abs(xp - x1) >= 4 || abs(yp - y1) >= 4))) {
				try2++;
				continue;
			}

			PlaceMonster(ActiveMonsterCount, typeIndex, { xp, yp });
			if (leader != nullptr) {
				auto &minion = Monsters[ActiveMonsterCount];
				minion.maxHitPoints *= 2;
				minion.hitPoints = minion.maxHitPoints;
				minion.intelligence = leader->intelligence;

				if (leashed) {
					minion.setLeader(leader);
				}

				if (minion.ai != AI_GARG) {
					minion.changeAnimationData(MonsterGraphic::Stand);
					minion.animInfo.currentFrame = GenerateRnd(minion.animInfo.numberOfFrames - 1);
					minion.flags &= ~MFLAG_ALLOW_SPECIAL;
					minion.mode = MonsterMode::Stand;
				}
			}
			ActiveMonsterCount++;
			placed++;
			j++;
		}

		if (placed >= num) {
			break;
		}
	}

	if (leashed) {
		leader->packSize = placed;
	}
}

size_t GetMonsterTypeIndex(_monster_id type)
{
	for (size_t i = 0; i < LevelMonsterTypeCount; i++) {
		if (LevelMonsterTypes[i].type == type)
			return i;
	}
	return LevelMonsterTypeCount;
}

void PlaceUniqueMonst(UniqueMonsterType uniqindex, size_t minionType, int bosspacksize)
{
	auto &monster = Monsters[ActiveMonsterCount];
	const auto &uniqueMonsterData = UniqueMonstersData[static_cast<size_t>(uniqindex)];

	int count = 0;
	Point position;
	while (true) {
		position = Point { GenerateRnd(80), GenerateRnd(80) } + Displacement { 16, 16 };
		int count2 = 0;
		for (int x = position.x - 3; x < position.x + 3; x++) {
			for (int y = position.y - 3; y < position.y + 3; y++) {
				if (InDungeonBounds({ x, y }) && CanPlaceMonster({ x, y })) {
					count2++;
				}
			}
		}

		if (count2 < 9) {
			count++;
			if (count < 1000) {
				continue;
			}
		}

		if (CanPlaceMonster(position)) {
			break;
		}
	}

	if (uniqindex == UniqueMonsterType::SnotSpill) {
		position = SetPiece.position.megaToWorld() + Displacement { 8, 12 };
	}
	if (uniqindex == UniqueMonsterType::WarlordOfBlood) {
		position = SetPiece.position.megaToWorld() + Displacement { 6, 7 };
	}
	if (uniqindex == UniqueMonsterType::Zhar) {
		for (int i = 0; i < themeCount; i++) {
			if (i == zharlib) {
				position = themeLoc[i].room.position.megaToWorld() + Displacement { 4, 4 };
				break;
			}
		}
	}
	if (setlevel) {
		if (uniqindex == UniqueMonsterType::Lazarus) {
			position = { 32, 46 };
		}
		if (uniqindex == UniqueMonsterType::RedVex) {
			position = { 40, 45 };
		}
		if (uniqindex == UniqueMonsterType::BlackJade) {
			position = { 38, 49 };
		}
		if (uniqindex == UniqueMonsterType::SkeletonKing) {
			position = { 35, 47 };
		}
	} else {
		if (uniqindex == UniqueMonsterType::Lazarus) {
			position = SetPiece.position.megaToWorld() + Displacement { 3, 6 };
		}
		if (uniqindex == UniqueMonsterType::RedVex) {
			position = SetPiece.position.megaToWorld() + Displacement { 5, 3 };
		}
		if (uniqindex == UniqueMonsterType::BlackJade) {
			position = SetPiece.position.megaToWorld() + Displacement { 5, 9 };
		}
	}
	if (uniqindex == UniqueMonsterType::Butcher) {
		position = SetPiece.position.megaToWorld() + Displacement { 4, 4 };
	}

	if (uniqindex == UniqueMonsterType::NaKrul) {
		if (UberRow == 0 || UberCol == 0) {
			UberDiabloMonsterIndex = -1;
			return;
		}
		position = { UberRow - 2, UberCol };
		UberDiabloMonsterIndex = static_cast<int>(ActiveMonsterCount);
	}
	const size_t typeIndex = GetMonsterTypeIndex(uniqueMonsterData.mtype);
	PlaceMonster(ActiveMonsterCount, typeIndex, position);
	ActiveMonsterCount++;

	PrepareUniqueMonst(monster, uniqindex, minionType, bosspacksize, uniqueMonsterData);
}

size_t AddMonsterType(_monster_id type, placeflag placeflag)
{
	const size_t typeIndex = GetMonsterTypeIndex(type);
	CMonster &monsterType = LevelMonsterTypes[typeIndex];

	if (typeIndex == LevelMonsterTypeCount) {
		LevelMonsterTypeCount++;
		monsterType.type = type;
		monstimgtot += MonstersData[type].image;
		InitMonsterGFX(monsterType);
		InitMonsterSND(monsterType);
	}

	monsterType.placeFlags |= placeflag;
	return typeIndex;
}

inline size_t AddMonsterType(UniqueMonsterType uniqueType, placeflag placeflag)
{
	return AddMonsterType(UniqueMonstersData[static_cast<size_t>(uniqueType)].mtype, placeflag);
}

void ClearMVars(Monster &monster)
{
	monster.var1 = 0;
	monster.var2 = 0;
	monster.var3 = 0;
	monster.position.temp = { 0, 0 };
}

void ClrAllMonsters()
{
	for (auto &monster : Monsters) {
		ClearMVars(monster);
		monster.goal = MonsterGoal::None;
		monster.mode = MonsterMode::Stand;
		monster.var1 = 0;
		monster.var2 = 0;
		monster.position.tile = { 0, 0 };
		monster.position.future = { 0, 0 };
		monster.position.old = { 0, 0 };
		monster.direction = static_cast<Direction>(GenerateRnd(8));
		monster.animInfo = {};
		monster.flags = 0;
		monster.isInvalid = false;
		monster.enemy = GenerateRnd(gbActivePlayers);
		monster.enemyPosition = Players[monster.enemy].position.future;
	}
}

void PlaceUniqueMonsters()
{
	for (size_t u = 0; UniqueMonstersData[u].mtype != -1; u++) {
		if (UniqueMonstersData[u].mlevel != currlevel)
			continue;

		const size_t minionType = GetMonsterTypeIndex(UniqueMonstersData[u].mtype);
		if (minionType == LevelMonsterTypeCount)
			continue;

		UniqueMonsterType uniqueType = static_cast<UniqueMonsterType>(u);
		if (uniqueType == UniqueMonsterType::Garbud && Quests[Q_GARBUD]._qactive == QUEST_NOTAVAIL)
			continue;
		if (uniqueType == UniqueMonsterType::Zhar && Quests[Q_ZHAR]._qactive == QUEST_NOTAVAIL)
			continue;
		if (uniqueType == UniqueMonsterType::SnotSpill && Quests[Q_LTBANNER]._qactive == QUEST_NOTAVAIL)
			continue;
		if (uniqueType == UniqueMonsterType::Lachdan && Quests[Q_VEIL]._qactive == QUEST_NOTAVAIL)
			continue;
		if (uniqueType == UniqueMonsterType::WarlordOfBlood && Quests[Q_WARLORD]._qactive == QUEST_NOTAVAIL)
			continue;

		PlaceUniqueMonst(uniqueType, minionType, 8);
	}
}

void PlaceQuestMonsters()
{
	if (!setlevel) {
		if (Quests[Q_BUTCHER].IsAvailable()) {
			PlaceUniqueMonst(UniqueMonsterType::Butcher, 0, 0);
		}

		if (currlevel == Quests[Q_SKELKING]._qlevel && gbIsMultiplayer) {
			for (size_t i = 0; i < LevelMonsterTypeCount; i++) {
				if (IsSkel(LevelMonsterTypes[i].type)) {
					PlaceUniqueMonst(UniqueMonsterType::SkeletonKing, i, 30);
					break;
				}
			}
		}

		if (Quests[Q_LTBANNER].IsAvailable()) {
			auto dunData = LoadFileInMem<uint16_t>("levels\\l1data\\banner1.dun");
			SetMapMonsters(dunData.get(), SetPiece.position.megaToWorld());
		}
		if (Quests[Q_BLOOD].IsAvailable()) {
			auto dunData = LoadFileInMem<uint16_t>("levels\\l2data\\blood2.dun");
			SetMapMonsters(dunData.get(), SetPiece.position.megaToWorld());
		}
		if (Quests[Q_BLIND].IsAvailable()) {
			auto dunData = LoadFileInMem<uint16_t>("levels\\l2data\\blind2.dun");
			SetMapMonsters(dunData.get(), SetPiece.position.megaToWorld());
		}
		if (Quests[Q_ANVIL].IsAvailable()) {
			auto dunData = LoadFileInMem<uint16_t>("levels\\l3data\\anvil.dun");
			SetMapMonsters(dunData.get(), SetPiece.position.megaToWorld() + Displacement { 2, 2 });
		}
		if (Quests[Q_WARLORD].IsAvailable()) {
			auto dunData = LoadFileInMem<uint16_t>("levels\\l4data\\warlord.dun");
			SetMapMonsters(dunData.get(), SetPiece.position.megaToWorld());
			AddMonsterType(UniqueMonsterType::WarlordOfBlood, PLACE_SCATTER);
		}
		if (Quests[Q_VEIL].IsAvailable()) {
			AddMonsterType(UniqueMonsterType::Lachdan, PLACE_SCATTER);
		}
		if (Quests[Q_ZHAR].IsAvailable() && zharlib == -1) {
			Quests[Q_ZHAR]._qactive = QUEST_NOTAVAIL;
		}

		if (currlevel == Quests[Q_BETRAYER]._qlevel && gbIsMultiplayer) {
			AddMonsterType(UniqueMonsterType::Lazarus, PLACE_UNIQUE);
			AddMonsterType(UniqueMonsterType::RedVex, PLACE_UNIQUE);
			PlaceUniqueMonst(UniqueMonsterType::Lazarus, 0, 0);
			PlaceUniqueMonst(UniqueMonsterType::RedVex, 0, 0);
			PlaceUniqueMonst(UniqueMonsterType::BlackJade, 0, 0);
			auto dunData = LoadFileInMem<uint16_t>("levels\\l4data\\vile1.dun");
			SetMapMonsters(dunData.get(), SetPiece.position.megaToWorld());
		}

		if (currlevel == 24) {
			UberDiabloMonsterIndex = -1;
			const size_t typeIndex = GetMonsterTypeIndex(MT_NAKRUL);
			if (typeIndex < LevelMonsterTypeCount) {
				for (size_t i = 0; i < ActiveMonsterCount; i++) {
					Monster &monster = Monsters[i];
					if (monster.isUnique() || monster.levelType == typeIndex) {
						UberDiabloMonsterIndex = static_cast<int>(i);
						break;
					}
				}
			}
			if (UberDiabloMonsterIndex == -1)
				PlaceUniqueMonst(UniqueMonsterType::NaKrul, 0, 0);
		}
	} else if (setlvlnum == SL_SKELKING) {
		PlaceUniqueMonst(UniqueMonsterType::SkeletonKing, 0, 0);
	}
}

void LoadDiabMonsts()
{
	{
		auto dunData = LoadFileInMem<uint16_t>("levels\\l4data\\diab1.dun");
		SetMapMonsters(dunData.get(), DiabloQuad1.megaToWorld());
	}
	{
		auto dunData = LoadFileInMem<uint16_t>("levels\\l4data\\diab2a.dun");
		SetMapMonsters(dunData.get(), DiabloQuad2.megaToWorld());
	}
	{
		auto dunData = LoadFileInMem<uint16_t>("levels\\l4data\\diab3a.dun");
		SetMapMonsters(dunData.get(), DiabloQuad3.megaToWorld());
	}
	{
		auto dunData = LoadFileInMem<uint16_t>("levels\\l4data\\diab4a.dun");
		SetMapMonsters(dunData.get(), DiabloQuad4.megaToWorld());
	}
}

void DeleteMonster(size_t activeIndex)
{
	const auto &monster = Monsters[ActiveMonsters[activeIndex]];
	if ((monster.flags & MFLAG_BERSERK) != 0) {
		AddUnLight(monster.lightId);
	}

	ActiveMonsterCount--;
	std::swap(ActiveMonsters[activeIndex], ActiveMonsters[ActiveMonsterCount]); // This ensures alive monsters are before ActiveMonsterCount in the array and any deleted monster after
}

void NewMonsterAnim(Monster &monster, MonsterGraphic graphic, Direction md, AnimationDistributionFlags flags = AnimationDistributionFlags::None, int8_t numSkippedFrames = 0, int8_t distributeFramesBeforeFrame = 0)
{
	const auto &animData = monster.type().getAnimData(graphic);
	monster.animInfo.setNewAnimation(animData.spritesForDirection(md), animData.frames, animData.rate, flags, numSkippedFrames, distributeFramesBeforeFrame);
	monster.flags &= ~(MFLAG_LOCK_ANIMATION | MFLAG_ALLOW_SPECIAL);
	monster.direction = md;
}

void StartMonsterGotHit(Monster &monster)
{
	if (monster.type().type != MT_GOLEM) {
		auto animationFlags = gGameLogicStep < GameLogicStep::ProcessMonsters ? AnimationDistributionFlags::ProcessAnimationPending : AnimationDistributionFlags::None;
		int8_t numSkippedFrames = (gbIsHellfire && monster.type().type == MT_DIABLO) ? 4 : 0;
		NewMonsterAnim(monster, MonsterGraphic::GotHit, monster.direction, animationFlags, numSkippedFrames);
		monster.mode = MonsterMode::HitRecovery;
	}
	monster.position.tile = monster.position.old;
	monster.position.future = monster.position.old;
	M_ClearSquares(monster);
	dMonster[monster.position.tile.x][monster.position.tile.y] = monster.getId() + 1;
}

bool IsRanged(Monster &monster)
{
	return IsAnyOf(monster.ai, AI_SKELBOW, AI_GOATBOW, AI_SUCC, AI_LAZHELP);
}

void UpdateEnemy(Monster &monster)
{
	Point target;
	int menemy = -1;
	int bestDist = -1;
	bool bestsameroom = false;
	const auto &position = monster.position.tile;
	if ((monster.flags & MFLAG_BERSERK) != 0 || (monster.flags & MFLAG_GOLEM) == 0) {
		for (size_t pnum = 0; pnum < Players.size(); pnum++) {
			Player &player = Players[pnum];
			if (!player.plractive || !player.isOnActiveLevel() || player._pLvlChanging
			    || (((player._pHitPoints >> 6) == 0) && gbIsMultiplayer))
				continue;
			bool sameroom = (dTransVal[position.x][position.y] == dTransVal[player.position.tile.x][player.position.tile.y]);
			int dist = position.WalkingDistance(player.position.tile);
			if ((sameroom && !bestsameroom)
			    || ((sameroom || !bestsameroom) && dist < bestDist)
			    || (menemy == -1)) {
				monster.flags &= ~MFLAG_TARGETS_MONSTER;
				menemy = static_cast<int>(pnum);
				target = player.position.future;
				bestDist = dist;
				bestsameroom = sameroom;
			}
		}
	}
	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		int monsterId = ActiveMonsters[i];
		auto &otherMonster = Monsters[monsterId];
		if (&otherMonster == &monster)
			continue;
		if ((otherMonster.hitPoints >> 6) <= 0)
			continue;
		if (otherMonster.position.tile == GolemHoldingCell)
			continue;
		if (M_Talker(otherMonster) && otherMonster.talkMsg != TEXT_NONE)
			continue;
		if (monster.isPlayerMinion() && otherMonster.isPlayerMinion()) // prevent golems from fighting each other
			continue;

		int dist = otherMonster.position.tile.WalkingDistance(position);
		if (((monster.flags & MFLAG_GOLEM) == 0
		        && (monster.flags & MFLAG_BERSERK) == 0
		        && dist >= 2
		        && !IsRanged(monster))
		    || ((monster.flags & MFLAG_GOLEM) == 0
		        && (monster.flags & MFLAG_BERSERK) == 0
		        && (otherMonster.flags & MFLAG_GOLEM) == 0)) {
			continue;
		}
		bool sameroom = dTransVal[position.x][position.y] == dTransVal[otherMonster.position.tile.x][otherMonster.position.tile.y];
		if ((sameroom && !bestsameroom)
		    || ((sameroom || !bestsameroom) && dist < bestDist)
		    || (menemy == -1)) {
			monster.flags |= MFLAG_TARGETS_MONSTER;
			menemy = monsterId;
			target = otherMonster.position.future;
			bestDist = dist;
			bestsameroom = sameroom;
		}
	}
	if (menemy != -1) {
		monster.flags &= ~MFLAG_NO_ENEMY;
		monster.enemy = menemy;
		monster.enemyPosition = target;
	} else {
		monster.flags |= MFLAG_NO_ENEMY;
	}
}

/**
 * @brief Make the AI wait a bit before thinking again
 * @param monster The monster that will wait
 * @param len
 */
void AiDelay(Monster &monster, int len)
{
	if (len <= 0) {
		return;
	}

	if (monster.ai == AI_LAZARUS) {
		return;
	}

	monster.var2 = len;
	monster.mode = MonsterMode::Delay;
}

/**
 * @brief Get the direction from the monster to its current enemy
 */
Direction GetMonsterDirection(Monster &monster)
{
	return GetDirection(monster.position.tile, monster.enemyPosition);
}

void StartSpecialStand(Monster &monster, Direction md)
{
	NewMonsterAnim(monster, MonsterGraphic::Special, md);
	monster.mode = MonsterMode::SpecialStand;
	monster.position.future = monster.position.tile;
	monster.position.old = monster.position.tile;
}

void WalkNorthwards(Monster &monster, int xadd, int yadd, Direction endDir)
{
	const auto fx = static_cast<WorldTileCoord>(xadd + monster.position.tile.x);
	const auto fy = static_cast<WorldTileCoord>(yadd + monster.position.tile.y);

	dMonster[fx][fy] = -(monster.getId() + 1);
	monster.mode = MonsterMode::MoveNorthwards;
	monster.position.old = monster.position.tile;
	monster.position.future = { fx, fy };
	monster.var1 = xadd;
	monster.var2 = yadd;
	monster.var3 = static_cast<int>(endDir);
	NewMonsterAnim(monster, MonsterGraphic::Walk, endDir, AnimationDistributionFlags::ProcessAnimationPending, -1);
}

void WalkSouthwards(Monster &monster, int xoff, int yoff, int xadd, int yadd, Direction endDir)
{
	const auto fx = static_cast<WorldTileCoord>(xadd + monster.position.tile.x);
	const auto fy = static_cast<WorldTileCoord>(yadd + monster.position.tile.y);

	dMonster[monster.position.tile.x][monster.position.tile.y] = -(monster.getId() + 1);
	monster.var1 = monster.position.tile.x;
	monster.var2 = monster.position.tile.y;
	monster.position.old = monster.position.tile;
	monster.position.tile = { fx, fy };
	monster.position.future = { fx, fy };
	dMonster[fx][fy] = monster.getId() + 1;
	if (monster.lightId != NO_LIGHT)
		ChangeLightXY(monster.lightId, monster.position.tile);
	monster.mode = MonsterMode::MoveSouthwards;
	monster.var3 = static_cast<int>(endDir);
	NewMonsterAnim(monster, MonsterGraphic::Walk, endDir, AnimationDistributionFlags::ProcessAnimationPending, -1);
}

void WalkSideways(Monster &monster, int xoff, int yoff, int xadd, int yadd, int mapx, int mapy, Direction endDir)
{
	const auto fx = static_cast<WorldTileCoord>(xadd + monster.position.tile.x);
	const auto fy = static_cast<WorldTileCoord>(yadd + monster.position.tile.y);
	const auto x = static_cast<WorldTileCoord>(mapx + monster.position.tile.x);
	const auto y = static_cast<WorldTileCoord>(mapy + monster.position.tile.y);

	if (monster.lightId != NO_LIGHT)
		ChangeLightXY(monster.lightId, { x, y });

	dMonster[monster.position.tile.x][monster.position.tile.y] = -(monster.getId() + 1);
	dMonster[fx][fy] = monster.getId() + 1;
	monster.position.temp = { x, y };
	monster.position.old = monster.position.tile;
	monster.position.future = { fx, fy };
	monster.mode = MonsterMode::MoveSideways;
	monster.var1 = fx;
	monster.var2 = fy;
	monster.var3 = static_cast<int>(endDir);
	NewMonsterAnim(monster, MonsterGraphic::Walk, endDir, AnimationDistributionFlags::ProcessAnimationPending, -1);
}

void StartAttack(Monster &monster)
{
	Direction md = GetMonsterDirection(monster);
	NewMonsterAnim(monster, MonsterGraphic::Attack, md, AnimationDistributionFlags::ProcessAnimationPending);
	monster.mode = MonsterMode::MeleeAttack;
	monster.position.future = monster.position.tile;
	monster.position.old = monster.position.tile;
}

void StartRangedAttack(Monster &monster, missile_id missileType, int dam)
{
	Direction md = GetMonsterDirection(monster);
	NewMonsterAnim(monster, MonsterGraphic::Attack, md, AnimationDistributionFlags::ProcessAnimationPending);
	monster.mode = MonsterMode::RangedAttack;
	monster.var1 = missileType;
	monster.var2 = dam;
	monster.position.future = monster.position.tile;
	monster.position.old = monster.position.tile;
}

void StartRangedSpecialAttack(Monster &monster, missile_id missileType, int dam)
{
	Direction md = GetMonsterDirection(monster);
	int8_t distributeFramesBeforeFrame = 0;
	if (monster.ai == AI_MEGA)
		distributeFramesBeforeFrame = monster.data().animFrameNumSpecial;
	NewMonsterAnim(monster, MonsterGraphic::Special, md, AnimationDistributionFlags::ProcessAnimationPending, 0, distributeFramesBeforeFrame);
	monster.mode = MonsterMode::SpecialRangedAttack;
	monster.var1 = missileType;
	monster.var2 = 0;
	monster.var3 = dam;
	monster.position.future = monster.position.tile;
	monster.position.old = monster.position.tile;
}

void StartSpecialAttack(Monster &monster)
{
	Direction md = GetMonsterDirection(monster);
	NewMonsterAnim(monster, MonsterGraphic::Special, md);
	monster.mode = MonsterMode::SpecialMeleeAttack;
	monster.position.future = monster.position.tile;
	monster.position.old = monster.position.tile;
}

void StartEating(Monster &monster)
{
	NewMonsterAnim(monster, MonsterGraphic::Special, monster.direction);
	monster.mode = MonsterMode::SpecialMeleeAttack;
	monster.position.future = monster.position.tile;
	monster.position.old = monster.position.tile;
}

void DiabloDeath(Monster &diablo, bool sendmsg)
{
	PlaySFX(USFX_DIABLOD);
	auto &quest = Quests[Q_DIABLO];
	quest._qactive = QUEST_DONE;
	if (sendmsg)
		NetSendCmdQuest(true, quest);
	sgbSaveSoundOn = gbSoundOn;
	gbProcessPlayers = false;
	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		int monsterId = ActiveMonsters[i];
		Monster &monster = Monsters[monsterId];
		if (monster.type().type == MT_DIABLO || diablo.activeForTicks == 0)
			continue;

		NewMonsterAnim(monster, MonsterGraphic::Death, monster.direction);
		monster.mode = MonsterMode::Death;
		monster.var1 = 0;
		monster.position.tile = monster.position.old;
		monster.position.future = monster.position.tile;
		M_ClearSquares(monster);
		dMonster[monster.position.tile.x][monster.position.tile.y] = monsterId + 1;
	}
	AddLight(diablo.position.tile, 8);
	DoVision(diablo.position.tile, 8, MAP_EXP_NONE, true);
	int dist = diablo.position.tile.WalkingDistance(ViewPosition);
	if (dist > 20)
		dist = 20;
	diablo.var3 = ViewPosition.x << 16;
	diablo.position.temp.x = ViewPosition.y << 16;
	diablo.position.temp.y = (int)((diablo.var3 - (diablo.position.tile.x << 16)) / (double)dist);
	if (!gbIsMultiplayer) {
		Player &myPlayer = *MyPlayer;
		myPlayer.pDiabloKillLevel = std::max(myPlayer.pDiabloKillLevel, static_cast<uint8_t>(sgGameInitInfo.nDifficulty + 1));
	}
}

void SpawnLoot(Monster &monster, bool sendmsg)
{
	if (monster.type().type == MT_HORKSPWN) {
		return;
	}

	if (Quests[Q_GARBUD].IsAvailable() && monster.uniqueType == UniqueMonsterType::Garbud) {
		CreateTypeItem(monster.position.tile + Displacement { 1, 1 }, true, ItemType::Mace, IMISC_NONE, sendmsg, false);
	} else if (monster.uniqueType == UniqueMonsterType::Defiler) {
		if (effect_is_playing(USFX_DEFILER8))
			stream_stop();
		Quests[Q_DEFILER]._qlog = false;
		SpawnMapOfDoom(monster.position.tile, sendmsg);
	} else if (monster.uniqueType == UniqueMonsterType::HorkDemon) {
		if (sgGameInitInfo.bTheoQuest != 0) {
			SpawnTheodore(monster.position.tile, sendmsg);
		} else {
			CreateAmulet(monster.position.tile, 13, sendmsg, false);
		}
	} else if (monster.type().type == MT_NAKRUL) {
		int nSFX = IsUberRoomOpened ? USFX_NAKRUL4 : USFX_NAKRUL5;
		if (sgGameInitInfo.bCowQuest != 0)
			nSFX = USFX_NAKRUL6;
		if (effect_is_playing(nSFX))
			stream_stop();
		Quests[Q_NAKRUL]._qlog = false;
		UberDiabloMonsterIndex = -2;
		CreateMagicWeapon(monster.position.tile, ItemType::Sword, ICURS_GREAT_SWORD, sendmsg, false);
		CreateMagicWeapon(monster.position.tile, ItemType::Staff, ICURS_WAR_STAFF, sendmsg, false);
		CreateMagicWeapon(monster.position.tile, ItemType::Bow, ICURS_LONG_WAR_BOW, sendmsg, false);
		CreateSpellBook(monster.position.tile, SPL_APOCA, sendmsg, false);
	} else if (!monster.isPlayerMinion()) {
		SpawnItem(monster, monster.position.tile, sendmsg);
	}
}

std::optional<Point> GetTeleportTile(const Monster &monster)
{
	int mx = monster.enemyPosition.x;
	int my = monster.enemyPosition.y;
	int rx = PickRandomlyAmong({ -1, 1 });
	int ry = PickRandomlyAmong({ -1, 1 });

	for (int j = -1; j <= 1; j++) {
		for (int k = -1; k < 1; k++) {
			if (j == 0 && k == 0)
				continue;
			int x = mx + rx * j;
			int y = my + ry * k;
			if (!InDungeonBounds({ x, y }) || x == monster.position.tile.x || y == monster.position.tile.y)
				continue;
			if (IsTileAvailable(monster, { x, y }))
				return Point { x, y };
		}
	}
	return {};
}

void Teleport(Monster &monster)
{
	if (monster.mode == MonsterMode::Petrified)
		return;

	std::optional<Point> position = GetTeleportTile(monster);
	if (!position)
		return;

	M_ClearSquares(monster);
	dMonster[monster.position.tile.x][monster.position.tile.y] = 0;
	dMonster[position->x][position->y] = monster.getId() + 1;
	monster.position.old = *position;
	monster.direction = GetMonsterDirection(monster);

	if (monster.lightId != NO_LIGHT) {
		ChangeLightXY(monster.lightId, *position);
	}
}

bool IsHardHit(Monster &target, unsigned dam)
{
	switch (target.type().type) {
	case MT_SNEAK:
	case MT_STALKER:
	case MT_UNSEEN:
	case MT_ILLWEAV:
		return true;
	default:
		return (dam >> 6) >= target.level(sgGameInitInfo.nDifficulty) + 3;
	}
}

void MonsterHitMonster(Monster &attacker, Monster &target, int dam)
{
	if (IsHardHit(target, dam)) {
		target.direction = Opposite(attacker.direction);
	}

	M_StartHit(target, dam);
}

void StartDeathFromMonster(Monster &attacker, Monster &target)
{
	Direction md = GetDirection(target.position.tile, attacker.position.tile);
	MonsterDeath(target, md, true);

	if (gbIsHellfire)
		M_StartStand(attacker, attacker.direction);
}

void StartFadein(Monster &monster, Direction md, bool backwards)
{
	NewMonsterAnim(monster, MonsterGraphic::Special, md);
	monster.mode = MonsterMode::FadeIn;
	monster.position.future = monster.position.tile;
	monster.position.old = monster.position.tile;
	monster.flags &= ~MFLAG_HIDDEN;
	if (backwards) {
		monster.flags |= MFLAG_LOCK_ANIMATION;
		monster.animInfo.currentFrame = monster.animInfo.numberOfFrames - 1;
	}
}

void StartFadeout(Monster &monster, Direction md, bool backwards)
{
	NewMonsterAnim(monster, MonsterGraphic::Special, md);
	monster.mode = MonsterMode::FadeOut;
	monster.position.future = monster.position.tile;
	monster.position.old = monster.position.tile;
	if (backwards) {
		monster.flags |= MFLAG_LOCK_ANIMATION;
		monster.animInfo.currentFrame = monster.animInfo.numberOfFrames - 1;
	}
}

/**
 * @brief Starts the monster healing procedure.
 *
 * The monster will be healed between 1.47% and 25% of its max HP. The healing amount is stored in _mVar1.
 *
 * This is only used by Gargoyles.
 *
 * @param monster The monster that will be healed.
 */
void StartHeal(Monster &monster)
{
	monster.changeAnimationData(MonsterGraphic::Special);
	monster.animInfo.currentFrame = monster.type().getAnimData(MonsterGraphic::Special).frames - 1;
	monster.flags |= MFLAG_LOCK_ANIMATION;
	monster.mode = MonsterMode::Heal;
	monster.var1 = monster.maxHitPoints / (16 * (GenerateRnd(5) + 4));
}

void SyncLightPosition(Monster &monster)
{
	Displacement offset = monster.position.CalculateWalkingOffset(monster.direction, monster.animInfo);
	ChangeLightOffset(monster.lightId, offset.screenToLight());
}

void MonsterIdle(Monster &monster)
{
	if (monster.type().type == MT_GOLEM)
		monster.changeAnimationData(MonsterGraphic::Walk);
	else
		monster.changeAnimationData(MonsterGraphic::Stand);

	if (monster.animInfo.isLastFrame())
		UpdateEnemy(monster);

	monster.var2++;
}

/**
 * @brief Continue movement towards new tile
 */
bool MonsterWalk(Monster &monster, MonsterMode variant)
{
	// Check if we reached new tile
	const bool isAnimationEnd = monster.animInfo.isLastFrame();
	if (isAnimationEnd) {
		switch (variant) {
		case MonsterMode::MoveNorthwards:
			dMonster[monster.position.tile.x][monster.position.tile.y] = 0;
			monster.position.tile.x += monster.var1;
			monster.position.tile.y += monster.var2;
			dMonster[monster.position.tile.x][monster.position.tile.y] = monster.getId() + 1;
			break;
		case MonsterMode::MoveSouthwards:
			dMonster[monster.var1][monster.var2] = 0;
			break;
		case MonsterMode::MoveSideways:
			dMonster[monster.position.tile.x][monster.position.tile.y] = 0;
			monster.position.tile = WorldTilePosition { static_cast<WorldTileCoord>(monster.var1), static_cast<WorldTileCoord>(monster.var2) };
			// dMonster is set here for backwards comparability, without it the monster would be invisible if loaded from a vanilla save.
			dMonster[monster.position.tile.x][monster.position.tile.y] = monster.getId() + 1;
			break;
		default:
			break;
		}
		if (monster.lightId != NO_LIGHT)
			ChangeLightXY(monster.lightId, monster.position.tile);
		M_StartStand(monster, monster.direction);
	} else { // We didn't reach new tile so update monster's "sub-tile" position
		if (monster.animInfo.tickCounterOfCurrentFrame == 0) {
			if (monster.animInfo.currentFrame == 0 && monster.type().type == MT_FLESTHNG)
				PlayEffect(monster, MonsterSound::Special);
		}
	}

	if (monster.lightId != NO_LIGHT) // BUGFIX: change uniqtype check to lightId check like it is in all other places (fixed)
		SyncLightPosition(monster);

	return isAnimationEnd;
}

void MonsterAttackMonster(Monster &attacker, Monster &target, int hper, int mind, int maxd)
{
	if (!target.isPossibleToHit())
		return;

	int hit = GenerateRnd(100);
	if (target.mode == MonsterMode::Petrified)
		hit = 0;
	if (target.tryLiftGargoyle())
		return;
	if (hit >= hper)
		return;

	int dam = (mind + GenerateRnd(maxd - mind + 1)) << 6;
	ApplyMonsterDamage(target, dam);

	if (attacker.isPlayerMinion()) {
		int playerId = attacker.getId();
		const Player &player = Players[playerId];
		target.tag(player);
	}

	if (target.hitPoints >> 6 <= 0) {
		StartDeathFromMonster(attacker, target);
	} else {
		MonsterHitMonster(attacker, target, dam);
	}

	if (target.activeForTicks == 0) {
		target.activeForTicks = UINT8_MAX;
		target.position.last = attacker.position.tile;
	}
}

void CheckReflect(Monster &monster, Player &player, int &dam)
{
	player.wReflections--;
	if (player.wReflections <= 0)
		NetSendCmdParam1(true, CMD_SETREFLECT, 0);
	// reflects 20-30% damage
	int mdam = dam * (GenerateRnd(10) + 20L) / 100;
	ApplyMonsterDamage(monster, mdam);
	dam = std::max(dam - mdam, 0);
	if (monster.hitPoints >> 6 <= 0)
		M_StartKill(monster, player);
	else
		M_StartHit(monster, player, mdam);
}

void MonsterAttackPlayer(Monster &monster, Player &player, int hit, int minDam, int maxDam)
{
	if (player._pHitPoints >> 6 <= 0 || player._pInvincible || HasAnyOf(player._pSpellFlags, SpellFlag::Etherealize))
		return;
	if (monster.position.tile.WalkingDistance(player.position.tile) >= 2)
		return;

	int hper = GenerateRnd(100);
#ifdef _DEBUG
	if (DebugGodMode)
		hper = 1000;
#endif
	int ac = player.GetArmor();
	if (HasAnyOf(player.pDamAcFlags, ItemSpecialEffectHf::ACAgainstDemons) && monster.data().monsterClass == MonsterClass::Demon)
		ac += 40;
	if (HasAnyOf(player.pDamAcFlags, ItemSpecialEffectHf::ACAgainstUndead) && monster.data().monsterClass == MonsterClass::Undead)
		ac += 20;
	hit += 2 * (monster.level(sgGameInitInfo.nDifficulty) - player._pLevel)
	    + 30
	    - ac;
	int minhit = 15;
	if (currlevel == 14)
		minhit = 20;
	if (currlevel == 15)
		minhit = 25;
	if (currlevel == 16)
		minhit = 30;
	hit = std::max(hit, minhit);
	int blkper = 100;
	if ((player._pmode == PM_STAND || player._pmode == PM_ATTACK) && player._pBlockFlag) {
		blkper = GenerateRnd(100);
	}
	int blk = player.GetBlockChance() - (monster.level(sgGameInitInfo.nDifficulty) * 2);
	blk = clamp(blk, 0, 100);
	if (hper >= hit)
		return;
	if (blkper < blk) {
		Direction dir = GetDirection(player.position.tile, monster.position.tile);
		StartPlrBlock(player, dir);
		if (&player == MyPlayer && player.wReflections > 0) {
			int dam = GenerateRnd(((maxDam - minDam) << 6) + 1) + (minDam << 6);
			dam = std::max(dam + (player._pIGetHit << 6), 64);
			CheckReflect(monster, player, dam);
		}
		return;
	}
	if (monster.type().type == MT_YZOMBIE && &player == MyPlayer) {
		if (player._pMaxHP > 64) {
			if (player._pMaxHPBase > 64) {
				player._pMaxHP -= 64;
				if (player._pHitPoints > player._pMaxHP) {
					player._pHitPoints = player._pMaxHP;
				}
				player._pMaxHPBase -= 64;
				if (player._pHPBase > player._pMaxHPBase) {
					player._pHPBase = player._pMaxHPBase;
				}
			}
		}
	}
	int dam = (minDam << 6) + GenerateRnd(((maxDam - minDam) << 6) + 1);
	dam = std::max(dam + (player._pIGetHit << 6), 64);
	if (&player == MyPlayer) {
		if (player.wReflections > 0)
			CheckReflect(monster, player, dam);
		ApplyPlrDamage(player, 0, 0, dam);
	}

	// Reflect can also kill a monster, so make sure the monster is still alive
	if (HasAnyOf(player._pIFlags, ItemSpecialEffect::Thorns) && monster.mode != MonsterMode::Death) {
		int mdam = (GenerateRnd(3) + 1) << 6;
		ApplyMonsterDamage(monster, mdam);
		if (monster.hitPoints >> 6 <= 0)
			M_StartKill(monster, player);
		else
			M_StartHit(monster, player, mdam);
	}

	if ((monster.flags & MFLAG_NOLIFESTEAL) == 0 && monster.type().type == MT_SKING && gbIsMultiplayer)
		monster.hitPoints += dam;
	if (player._pHitPoints >> 6 <= 0) {
		if (gbIsHellfire)
			M_StartStand(monster, monster.direction);
		return;
	}
	StartPlrHit(player, dam, false);
	if ((monster.flags & MFLAG_KNOCKBACK) != 0) {
		if (player._pmode != PM_GOTHIT)
			StartPlrHit(player, 0, true);

		Point newPosition = player.position.tile + monster.direction;
		if (PosOkPlayer(player, newPosition)) {
			player.position.tile = newPosition;
			FixPlayerLocation(player, player._pdir);
			FixPlrWalkTags(player);
			dPlayer[newPosition.x][newPosition.y] = player.getId() + 1;
			SetPlayerOld(player);
		}
	}
}

void MonsterAttackEnemy(Monster &monster, int hit, int minDam, int maxDam)
{
	if ((monster.flags & MFLAG_TARGETS_MONSTER) != 0)
		MonsterAttackMonster(monster, Monsters[monster.enemy], hit, minDam, maxDam);
	else
		MonsterAttackPlayer(monster, Players[monster.enemy], hit, minDam, maxDam);
}

bool MonsterAttack(Monster &monster)
{
	if (monster.animInfo.currentFrame == monster.data().animFrameNum - 1) {
		MonsterAttackEnemy(monster, monster.toHit, monster.minDamage, monster.maxDamage);
		if (monster.ai != AI_SNAKE)
			PlayEffect(monster, MonsterSound::Attack);
	}
	if (IsAnyOf(monster.type().type, MT_NMAGMA, MT_YMAGMA, MT_BMAGMA, MT_WMAGMA) && monster.animInfo.currentFrame == 8) {
		MonsterAttackEnemy(monster, monster.toHit + 10, monster.minDamage - 2, monster.maxDamage - 2);

		PlayEffect(monster, MonsterSound::Attack);
	}
	if (IsAnyOf(monster.type().type, MT_STORM, MT_RSTORM, MT_STORML, MT_MAEL) && monster.animInfo.currentFrame == 12) {
		MonsterAttackEnemy(monster, monster.toHit - 20, monster.minDamage + 4, monster.maxDamage + 4);

		PlayEffect(monster, MonsterSound::Attack);
	}
	if (monster.ai == AI_SNAKE && monster.animInfo.currentFrame == 0)
		PlayEffect(monster, MonsterSound::Attack);
	if (monster.animInfo.isLastFrame()) {
		M_StartStand(monster, monster.direction);
		return true;
	}

	return false;
}

bool MonsterRangedAttack(Monster &monster)
{
	if (monster.animInfo.currentFrame == monster.data().animFrameNum - 1) {
		const auto &missileType = static_cast<missile_id>(monster.var1);
		if (missileType != MIS_NULL) {
			int multimissiles = 1;
			if (missileType == MIS_CBOLT)
				multimissiles = 3;
			for (int mi = 0; mi < multimissiles; mi++) {
				AddMissile(
				    monster.position.tile,
				    monster.enemyPosition,
				    monster.direction,
				    missileType,
				    TARGET_PLAYERS,
				    monster.getId(),
				    monster.var2,
				    0);
			}
		}
		PlayEffect(monster, MonsterSound::Attack);
	}

	if (monster.animInfo.isLastFrame()) {
		M_StartStand(monster, monster.direction);
		return true;
	}

	return false;
}

bool MonsterRangedSpecialAttack(Monster &monster)
{
	if (monster.animInfo.currentFrame == monster.data().animFrameNumSpecial - 1 && monster.animInfo.tickCounterOfCurrentFrame == 0 && (monster.ai != AI_MEGA || monster.var2 == 0)) {
		if (AddMissile(
		        monster.position.tile,
		        monster.enemyPosition,
		        monster.direction,
		        static_cast<missile_id>(monster.var1),
		        TARGET_PLAYERS,
		        monster.getId(),
		        monster.var3,
		        0)
		    != nullptr) {
			PlayEffect(monster, MonsterSound::Special);
		}
	}

	if (monster.ai == AI_MEGA && monster.animInfo.currentFrame == monster.data().animFrameNumSpecial - 1) {
		if (monster.var2++ == 0) {
			monster.flags |= MFLAG_ALLOW_SPECIAL;
		} else if (monster.var2 == 15) {
			monster.flags &= ~MFLAG_ALLOW_SPECIAL;
		}
	}

	if (monster.animInfo.isLastFrame()) {
		M_StartStand(monster, monster.direction);
		return true;
	}

	return false;
}

bool MonsterSpecialAttack(Monster &monster)
{
	if (monster.animInfo.currentFrame == monster.data().animFrameNumSpecial - 1) {
		MonsterAttackEnemy(monster, monster.toHitSpecial(sgGameInitInfo.nDifficulty), monster.minDamageSpecial, monster.maxDamageSpecial);
	}

	if (monster.animInfo.isLastFrame()) {
		M_StartStand(monster, monster.direction);
		return true;
	}

	return false;
}

bool MonsterFadein(Monster &monster)
{
	if (((monster.flags & MFLAG_LOCK_ANIMATION) == 0 || monster.animInfo.currentFrame != 0)
	    && ((monster.flags & MFLAG_LOCK_ANIMATION) != 0 || monster.animInfo.currentFrame != monster.animInfo.numberOfFrames - 1)) {
		return false;
	}

	M_StartStand(monster, monster.direction);
	monster.flags &= ~MFLAG_LOCK_ANIMATION;

	return true;
}

bool MonsterFadeout(Monster &monster)
{
	if (((monster.flags & MFLAG_LOCK_ANIMATION) == 0 || monster.animInfo.currentFrame != 0)
	    && ((monster.flags & MFLAG_LOCK_ANIMATION) != 0 || monster.animInfo.currentFrame != monster.animInfo.numberOfFrames - 1)) {
		return false;
	}

	monster.flags &= ~MFLAG_LOCK_ANIMATION;
	monster.flags |= MFLAG_HIDDEN;

	M_StartStand(monster, monster.direction);

	return true;
}

/**
 * @brief Applies the healing effect on the monster.
 *
 * This is triggered by StartHeal()
 *
 * @param monster The monster that will be healed.
 * @return
 */
void MonsterHeal(Monster &monster)
{
	if ((monster.flags & MFLAG_NOHEAL) != 0) {
		monster.flags &= ~MFLAG_ALLOW_SPECIAL;
		monster.mode = MonsterMode::SpecialMeleeAttack;
		return;
	}

	if (monster.animInfo.currentFrame == 0) {
		monster.flags &= ~MFLAG_LOCK_ANIMATION;
		monster.flags |= MFLAG_ALLOW_SPECIAL;
		if (monster.var1 + monster.hitPoints < monster.maxHitPoints) {
			monster.hitPoints = monster.var1 + monster.hitPoints;
		} else {
			monster.hitPoints = monster.maxHitPoints;
			monster.flags &= ~MFLAG_ALLOW_SPECIAL;
			monster.mode = MonsterMode::SpecialMeleeAttack;
		}
	}
}

void MonsterTalk(Monster &monster)
{
	M_StartStand(monster, monster.direction);
	monster.goal = MonsterGoal::Talking;
	if (effect_is_playing(Speeches[monster.talkMsg].sfxnr))
		return;
	InitQTextMsg(monster.talkMsg);
	if (monster.uniqueType == UniqueMonsterType::Garbud) {
		if (monster.talkMsg == TEXT_GARBUD1) {
			Quests[Q_GARBUD]._qactive = QUEST_ACTIVE;
			Quests[Q_GARBUD]._qlog = true; // BUGFIX: (?) for other quests qactive and qlog go together, maybe this should actually go into the if above (fixed)
		}
		if (monster.talkMsg == TEXT_GARBUD2 && (monster.flags & MFLAG_QUEST_COMPLETE) == 0) {
			SpawnItem(monster, monster.position.tile + Displacement { 1, 1 }, true);
			monster.flags |= MFLAG_QUEST_COMPLETE;
		}
	}
	if (monster.uniqueType == UniqueMonsterType::Zhar
	    && monster.talkMsg == TEXT_ZHAR1
	    && (monster.flags & MFLAG_QUEST_COMPLETE) == 0) {
		Quests[Q_ZHAR]._qactive = QUEST_ACTIVE;
		Quests[Q_ZHAR]._qlog = true;
		CreateTypeItem(monster.position.tile + Displacement { 1, 1 }, false, ItemType::Misc, IMISC_BOOK, true, false);
		monster.flags |= MFLAG_QUEST_COMPLETE;
	}
	if (monster.uniqueType == UniqueMonsterType::SnotSpill) {
		if (monster.talkMsg == TEXT_BANNER10 && (monster.flags & MFLAG_QUEST_COMPLETE) == 0) {
			ObjChangeMap(SetPiece.position.x, SetPiece.position.y, SetPiece.position.x + (SetPiece.size.width / 2) + 2, SetPiece.position.y + (SetPiece.size.height / 2) - 2);
			auto tren = TransVal;
			TransVal = 9;
			DRLG_MRectTrans({ SetPiece.position, WorldTileSize(SetPiece.size.width / 2 + 4, SetPiece.size.height / 2) });
			TransVal = tren;
			Quests[Q_LTBANNER]._qvar1 = 2;
			if (Quests[Q_LTBANNER]._qactive == QUEST_INIT)
				Quests[Q_LTBANNER]._qactive = QUEST_ACTIVE;
			monster.flags |= MFLAG_QUEST_COMPLETE;
		}
		if (Quests[Q_LTBANNER]._qvar1 < 2) {
			app_fatal(StrCat("SS Talk = ", monster.talkMsg, ", Flags = ", monster.flags));
		}
	}
	if (monster.uniqueType == UniqueMonsterType::Lachdan) {
		if (monster.talkMsg == TEXT_VEIL9) {
			Quests[Q_VEIL]._qactive = QUEST_ACTIVE;
			Quests[Q_VEIL]._qlog = true;
		}
		if (monster.talkMsg == TEXT_VEIL11 && (monster.flags & MFLAG_QUEST_COMPLETE) == 0) {
			SpawnUnique(UITEM_STEELVEIL, monster.position.tile + Direction::South);
			monster.flags |= MFLAG_QUEST_COMPLETE;
		}
	}
	if (monster.uniqueType == UniqueMonsterType::WarlordOfBlood)
		Quests[Q_WARLORD]._qvar1 = 2;
	if (monster.uniqueType == UniqueMonsterType::Lazarus && gbIsMultiplayer) {
		Quests[Q_BETRAYER]._qvar1 = 6;
		monster.goal = MonsterGoal::Normal;
		monster.activeForTicks = UINT8_MAX;
		monster.talkMsg = TEXT_NONE;
	}
}

bool MonsterGotHit(Monster &monster)
{
	if (monster.animInfo.isLastFrame()) {
		M_StartStand(monster, monster.direction);

		return true;
	}

	return false;
}

void ReleaseMinions(const Monster &leader)
{
	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		auto &minion = Monsters[ActiveMonsters[i]];
		if (minion.leaderRelation == LeaderRelation::Leashed && minion.getLeader() == &leader) {
			minion.setLeader(nullptr);
		}
	}
}

void ShrinkLeaderPacksize(const Monster &monster)
{
	if (monster.leaderRelation == LeaderRelation::Leashed) {
		monster.getLeader()->packSize--;
	}
}

void MonsterDeath(Monster &monster)
{
	monster.var1++;
	if (monster.type().type == MT_DIABLO) {
		if (monster.position.tile.x < ViewPosition.x) {
			ViewPosition.x--;
		} else if (monster.position.tile.x > ViewPosition.x) {
			ViewPosition.x++;
		}

		if (monster.position.tile.y < ViewPosition.y) {
			ViewPosition.y--;
		} else if (monster.position.tile.y > ViewPosition.y) {
			ViewPosition.y++;
		}

		if (monster.var1 == 140)
			PrepDoEnding();
	} else if (monster.animInfo.isLastFrame()) {
		if (monster.isUnique())
			AddCorpse(monster.position.tile, monster.corpseId, monster.direction);
		else
			AddCorpse(monster.position.tile, monster.type().corpseId, monster.direction);

		dMonster[monster.position.tile.x][monster.position.tile.y] = 0;
		monster.isInvalid = true;

		M_UpdateRelations(monster);
	}
}

bool MonsterSpecialStand(Monster &monster)
{
	if (monster.animInfo.currentFrame == monster.data().animFrameNumSpecial - 1)
		PlayEffect(monster, MonsterSound::Special);

	if (monster.animInfo.isLastFrame()) {
		M_StartStand(monster, monster.direction);
		return true;
	}

	return false;
}

bool MonsterDelay(Monster &monster)
{
	monster.changeAnimationData(MonsterGraphic::Stand, GetMonsterDirection(monster));
	if (monster.ai == AI_LAZARUS) {
		if (monster.var2 > 8 || monster.var2 < 0)
			monster.var2 = 8;
	}

	if (monster.var2-- == 0) {
		int oFrame = monster.animInfo.currentFrame;
		M_StartStand(monster, monster.direction);
		monster.animInfo.currentFrame = oFrame;
		return true;
	}

	return false;
}

void MonsterPetrified(Monster &monster)
{
	if (monster.hitPoints <= 0) {
		dMonster[monster.position.tile.x][monster.position.tile.y] = 0;
		monster.isInvalid = true;
	}
}

Monster *AddSkeleton(Point position, Direction dir, bool inMap)
{
	size_t typeCount = 0;
	size_t skeletonIndexes[SkeletonTypes.size()];
	for (size_t i = 0; i < LevelMonsterTypeCount; i++) {
		if (IsSkel(LevelMonsterTypes[i].type)) {
			skeletonIndexes[typeCount++] = i;
		}
	}

	if (typeCount == 0) {
		return nullptr;
	}

	const size_t typeIndex = skeletonIndexes[GenerateRnd(typeCount)];
	return AddMonster(position, dir, typeIndex, inMap);
}

void SpawnSkeleton(Point position, Direction dir)
{
	Monster *skeleton = AddSkeleton(position, dir, true);
	if (skeleton != nullptr)
		StartSpecialStand(*skeleton, dir);
}

bool IsLineNotSolid(Point startPoint, Point endPoint)
{
	return LineClear(IsTileNotSolid, startPoint, endPoint);
}

void FollowTheLeader(Monster &monster)
{
	if (monster.leaderRelation != LeaderRelation::Leashed)
		return;

	Monster *leader = monster.getLeader();
	if (leader == nullptr)
		return;

	if (monster.activeForTicks >= leader->activeForTicks)
		return;

	monster.position.last = leader->position.tile;
	monster.activeForTicks = leader->activeForTicks - 1;
}

void GroupUnity(Monster &monster)
{
	if (monster.leaderRelation == LeaderRelation::None)
		return;

	// No unique monster would be a minion of someone else!
	assert(!monster.isUnique());

	// Someone with a leaderRelation should have a leader, if we end up trying to access a nullptr then the relation was already broken...

	auto &leader = *monster.getLeader();
	if (IsLineNotSolid(monster.position.tile, leader.position.future)) {
		if (monster.leaderRelation == LeaderRelation::Separated
		    && monster.position.tile.WalkingDistance(leader.position.future) < 4) {
			// Reunite the separated monster with the pack
			leader.packSize++;
			monster.leaderRelation = LeaderRelation::Leashed;
		}
	} else if (monster.leaderRelation == LeaderRelation::Leashed) {
		leader.packSize--;
		monster.leaderRelation = LeaderRelation::Separated;
	}

	if (monster.leaderRelation == LeaderRelation::Leashed) {
		if (monster.activeForTicks > leader.activeForTicks) {
			leader.position.last = monster.position.tile;
			leader.activeForTicks = monster.activeForTicks - 1;
		}
		if (leader.ai == AI_GARG && (leader.flags & MFLAG_ALLOW_SPECIAL) != 0) {
			leader.flags &= ~MFLAG_ALLOW_SPECIAL;
			leader.mode = MonsterMode::SpecialMeleeAttack;
		}
	}
}

bool RandomWalk(Monster &monster, Direction md)
{
	Direction mdtemp = md;

	bool ok = DirOK(monster, md);
	if (FlipCoin())
		ok = ok || (md = Right(mdtemp), DirOK(monster, md)) || (md = Left(mdtemp), DirOK(monster, md));
	else
		ok = ok || (md = Left(mdtemp), DirOK(monster, md)) || (md = Right(mdtemp), DirOK(monster, md));
	if (FlipCoin()) {
		ok = ok
		    || (md = Left(Left(mdtemp)), DirOK(monster, md))
		    || (md = Right(Right(mdtemp)), DirOK(monster, md));
	} else {
		ok = ok
		    || (md = Right(Right(mdtemp)), DirOK(monster, md))
		    || (md = Left(Left(mdtemp)), DirOK(monster, md));
	}
	if (ok)
		Walk(monster, md);
	return ok;
}

bool RandomWalk2(Monster &monster, Direction md)
{
	Direction mdtemp = md;
	bool ok = DirOK(monster, md); // Can we continue in the same direction

	// Randomly go left or right
	if (FlipCoin()) {
		ok = ok || (mdtemp = Right(md), DirOK(monster, Right(md))) || (mdtemp = Left(md), DirOK(monster, Left(md)));
	} else {
		ok = ok || (mdtemp = Left(md), DirOK(monster, Left(md))) || (mdtemp = Right(md), DirOK(monster, Right(md)));
	}

	if (ok)
		Walk(monster, mdtemp);

	return ok;
}

/**
 * @brief Check if a tile is affected by a spell we are vunerable to
 */
bool IsTileSafe(const Monster &monster, Point position)
{
	if (!TileContainsMissile(position)) {
		return true;
	}

	bool fearsFire = (monster.resistance & IMMUNE_FIRE) == 0 || monster.type().type == MT_DIABLO;
	bool fearsLightning = (monster.resistance & IMMUNE_LIGHTNING) == 0 || monster.type().type == MT_DIABLO;

	for (auto &missile : Missiles) {
		if (missile.position.tile == position) {
			if (fearsFire && missile._mitype == MIS_FIREWALL) {
				return false;
			}
			if (fearsLightning && missile._mitype == MIS_LIGHTWALL) {
				return false;
			}
		}
	}

	return true;
}

/**
 * @brief Check that the given tile is not currently blocked
 */
bool IsTileAvailable(Point position)
{
	if (dPlayer[position.x][position.y] != 0 || dMonster[position.x][position.y] != 0)
		return false;

	if (!IsTileWalkable(position))
		return false;

	return true;
}

/**
 * @brief If a monster can access the given tile (possibly by opening a door)
 */
bool IsTileAccessible(const Monster &monster, Point position)
{
	if (dPlayer[position.x][position.y] != 0 || dMonster[position.x][position.y] != 0)
		return false;

	if (!IsTileWalkable(position, (monster.flags & MFLAG_CAN_OPEN_DOOR) != 0))
		return false;

	return IsTileSafe(monster, position);
}

bool AiPlanWalk(Monster &monster)
{
	int8_t path[MaxPathLength];

	/** Maps from walking path step to facing direction. */
	const Direction plr2monst[9] = { Direction::South, Direction::NorthEast, Direction::NorthWest, Direction::SouthEast, Direction::SouthWest, Direction::North, Direction::East, Direction::South, Direction::West };

	if (FindPath([&monster](Point position) { return IsTileAccessible(monster, position); }, monster.position.tile, monster.enemyPosition, path) == 0) {
		return false;
	}

	RandomWalk(monster, plr2monst[path[0]]);
	return true;
}

Direction Turn(Direction direction, bool turnLeft)
{
	return turnLeft ? Left(direction) : Right(direction);
}

bool RoundWalk(Monster &monster, Direction direction, int8_t *dir)
{
	Direction turn45deg = Turn(direction, *dir != 0);
	Direction turn90deg = Turn(turn45deg, *dir != 0);

	// Turn 90 degrees
	if (Walk(monster, turn90deg)) {
		return true;
	}

	// Only do a small turn
	if (Walk(monster, turn45deg)) {
		return true;
	}

	// Continue straight
	if (Walk(monster, direction)) {
		return true;
	}

	// Try 90 degrees in the opposite than desired direction
	*dir = (*dir == 0) ? 1 : 0;
	return RandomWalk(monster, Opposite(turn90deg));
}

bool AiPlanPath(Monster &monster)
{
	if (monster.type().type != MT_GOLEM) {
		if (monster.activeForTicks == 0)
			return false;
		if (monster.mode != MonsterMode::Stand)
			return false;
		if (IsNoneOf(monster.goal, MonsterGoal::Normal, MonsterGoal::Move, MonsterGoal::Attack))
			return false;
		if (monster.position.tile == GolemHoldingCell)
			return false;
	}

	bool clear = LineClear(
	    [&monster](Point position) { return IsTileAvailable(monster, position); },
	    monster.position.tile,
	    monster.enemyPosition);
	if (!clear || (monster.pathCount >= 5 && monster.pathCount < 8)) {
		if ((monster.flags & MFLAG_CAN_OPEN_DOOR) != 0)
			MonstCheckDoors(monster);
		monster.pathCount++;
		if (monster.pathCount < 5)
			return false;
		if (AiPlanWalk(monster))
			return true;
	}

	if (monster.type().type != MT_GOLEM)
		monster.pathCount = 0;

	return false;
}

void AiAvoidance(Monster &monster)
{
	if (monster.mode != MonsterMode::Stand || monster.activeForTicks == 0) {
		return;
	}

	Direction md = GetDirection(monster.position.tile, monster.position.last);
	if (monster.activeForTicks < UINT8_MAX)
		MonstCheckDoors(monster);
	int v = GenerateRnd(100);
	unsigned distanceToEnemy = monster.distanceToEnemy();
	if (distanceToEnemy >= 2 && monster.activeForTicks == UINT8_MAX && dTransVal[monster.position.tile.x][monster.position.tile.y] == dTransVal[monster.enemyPosition.x][monster.enemyPosition.y]) {
		if (monster.goal == MonsterGoal::Move || (distanceToEnemy >= 4 && FlipCoin(4))) {
			if (monster.goal != MonsterGoal::Move) {
				monster.goalVar1 = 0;
				monster.goalVar2 = GenerateRnd(2);
			}
			monster.goal = MonsterGoal::Move;
			if ((monster.goalVar1++ >= static_cast<int>(2 * distanceToEnemy) && DirOK(monster, md)) || dTransVal[monster.position.tile.x][monster.position.tile.y] != dTransVal[monster.enemyPosition.x][monster.enemyPosition.y]) {
				monster.goal = MonsterGoal::Normal;
			} else if (!RoundWalk(monster, md, &monster.goalVar2)) {
				AiDelay(monster, GenerateRnd(10) + 10);
			}
		}
	} else {
		monster.goal = MonsterGoal::Normal;
	}
	if (monster.goal == MonsterGoal::Normal) {
		if (distanceToEnemy >= 2) {
			if ((monster.var2 > 20 && v < 2 * monster.intelligence + 28)
			    || (IsMonsterModeMove(static_cast<MonsterMode>(monster.var1))
			        && monster.var2 == 0
			        && v < 2 * monster.intelligence + 78)) {
				RandomWalk(monster, md);
			}
		} else if (v < 2 * monster.intelligence + 23) {
			monster.direction = md;
			if (IsAnyOf(monster.ai, AI_GOATMC, AI_GARBUD) && monster.hitPoints < (monster.maxHitPoints / 2) && !FlipCoin())
				StartSpecialAttack(monster);
			else
				StartAttack(monster);
		}
	}

	monster.checkStandAnimationIsLoaded(md);
}

missile_id GetMissileType(_mai_id ai)
{
	switch (ai) {
	case AI_GOATMC:
		return MIS_ARROW;
	case AI_SUCC:
	case AI_LAZHELP:
		return MIS_FLARE;
	case AI_ACID:
	case AI_ACIDUNIQ:
		return MIS_ACID;
	case AI_FIREBAT:
		return MIS_FIREBOLT;
	case AI_TORCHANT:
		return MIS_FIREBALL;
	case AI_LICH:
		return MIS_LICH;
	case AI_ARCHLICH:
		return MIS_ARCHLICH;
	case AI_PSYCHORB:
		return MIS_PSYCHORB;
	case AI_NECROMORB:
		return MIS_NECROMORB;
	case AI_MAGMA:
		return MIS_MAGMABALL;
	case AI_STORM:
		return MIS_LIGHTCTRL2;
	case AI_DIABLO:
		return MIS_DIABAPOCA;
	case AI_BONEDEMON:
		return MIS_BONEDEMON;
	default:
		return MIS_ARROW;
	}
}

void AiRanged(Monster &monster)
{
	if (monster.mode != MonsterMode::Stand) {
		return;
	}

	if (monster.activeForTicks == UINT8_MAX || (monster.flags & MFLAG_TARGETS_MONSTER) != 0) {
		Direction md = GetMonsterDirection(monster);
		if (monster.activeForTicks < UINT8_MAX)
			MonstCheckDoors(monster);
		monster.direction = md;
		if (static_cast<MonsterMode>(monster.var1) == MonsterMode::RangedAttack) {
			AiDelay(monster, GenerateRnd(20));
		} else if (monster.distanceToEnemy() < 4) {
			if (GenerateRnd(100) < 10 * (monster.intelligence + 7))
				RandomWalk(monster, Opposite(md));
		}
		if (monster.mode == MonsterMode::Stand) {
			if (LineClearMissile(monster.position.tile, monster.enemyPosition)) {
				missile_id missileType = GetMissileType(monster.ai);
				if (monster.ai == AI_ACIDUNIQ)
					StartRangedSpecialAttack(monster, missileType, 4);
				else
					StartRangedAttack(monster, missileType, 4);
			} else {
				monster.checkStandAnimationIsLoaded(md);
			}
		}
		return;
	}

	if (monster.activeForTicks != 0) {
		Direction md = GetDirection(monster.position.tile, monster.position.last);
		RandomWalk(monster, md);
	}
}

void AiRangedAvoidance(Monster &monster)
{
	if (monster.mode != MonsterMode::Stand || monster.activeForTicks == 0) {
		return;
	}

	Direction md = GetDirection(monster.position.tile, monster.position.last);
	if (IsAnyOf(monster.ai, AI_MAGMA, AI_STORM, AI_BONEDEMON) && monster.activeForTicks < UINT8_MAX)
		MonstCheckDoors(monster);
	int lessmissiles = (monster.ai == AI_ACID) ? 1 : 0;
	int dam = (monster.ai == AI_DIABLO) ? 40 : 4;
	missile_id missileType = GetMissileType(monster.ai);
	int v = GenerateRnd(10000);
	unsigned distanceToEnemy = monster.distanceToEnemy();
	if (distanceToEnemy >= 2 && monster.activeForTicks == UINT8_MAX && dTransVal[monster.position.tile.x][monster.position.tile.y] == dTransVal[monster.enemyPosition.x][monster.enemyPosition.y]) {
		if (monster.goal == MonsterGoal::Move || (distanceToEnemy >= 3 && FlipCoin(4 << lessmissiles))) {
			if (monster.goal != MonsterGoal::Move) {
				monster.goalVar1 = 0;
				monster.goalVar2 = GenerateRnd(2);
			}
			monster.goal = MonsterGoal::Move;
			if (monster.goalVar1++ >= static_cast<int>(2 * distanceToEnemy) && DirOK(monster, md)) {
				monster.goal = MonsterGoal::Normal;
			} else if (v < (500 * (monster.intelligence + 1) >> lessmissiles)
			    && (LineClearMissile(monster.position.tile, monster.enemyPosition))) {
				StartRangedSpecialAttack(monster, missileType, dam);
			} else {
				RoundWalk(monster, md, &monster.goalVar2);
			}
		}
	} else {
		monster.goal = MonsterGoal::Normal;
	}
	if (monster.goal == MonsterGoal::Normal) {
		if (((distanceToEnemy >= 3 && v < ((500 * (monster.intelligence + 2)) >> lessmissiles))
		        || v < ((500 * (monster.intelligence + 1)) >> lessmissiles))
		    && LineClearMissile(monster.position.tile, monster.enemyPosition)) {
			StartRangedSpecialAttack(monster, missileType, dam);
		} else if (distanceToEnemy >= 2) {
			v = GenerateRnd(100);
			if (v < 1000 * (monster.intelligence + 5)
			    || (IsMonsterModeMove(static_cast<MonsterMode>(monster.var1))
			        && monster.var2 == 0
			        && v < 1000 * (monster.intelligence + 8))) {
				RandomWalk(monster, md);
			}
		} else if (v < 1000 * (monster.intelligence + 6)) {
			monster.direction = md;
			StartAttack(monster);
		}
	}
	if (monster.mode == MonsterMode::Stand) {
		AiDelay(monster, GenerateRnd(10) + 5);
	}
}

void ZombieAi(Monster &monster)
{
	if (monster.mode != MonsterMode::Stand) {
		return;
	}

	if (!IsTileVisible(monster.position.tile)) {
		return;
	}

	if (GenerateRnd(100) < 2 * monster.intelligence + 10) {
		int dist = monster.enemyPosition.WalkingDistance(monster.position.tile);
		if (dist >= 2) {
			if (dist >= 2 * monster.intelligence + 4) {
				Direction md = monster.direction;
				if (GenerateRnd(100) < 2 * monster.intelligence + 20) {
					md = static_cast<Direction>(GenerateRnd(8));
				}
				Walk(monster, md);
			} else {
				RandomWalk(monster, GetMonsterDirection(monster));
			}
		} else {
			StartAttack(monster);
		}
	}

	monster.checkStandAnimationIsLoaded(monster.direction);
}

void OverlordAi(Monster &monster)
{
	if (monster.mode != MonsterMode::Stand || monster.activeForTicks == 0) {
		return;
	}

	Direction md = GetMonsterDirection(monster);
	monster.direction = md;
	int v = GenerateRnd(100);
	if (monster.distanceToEnemy() >= 2) {
		if ((monster.var2 > 20 && v < 4 * monster.intelligence + 20)
		    || (IsMonsterModeMove(static_cast<MonsterMode>(monster.var1))
		        && monster.var2 == 0
		        && v < 4 * monster.intelligence + 70)) {
			RandomWalk(monster, md);
		}
	} else if (v < 4 * monster.intelligence + 15) {
		StartAttack(monster);
	} else if (v < 4 * monster.intelligence + 20) {
		StartSpecialAttack(monster);
	}

	monster.checkStandAnimationIsLoaded(md);
}

void SkeletonAi(Monster &monster)
{
	if (monster.mode != MonsterMode::Stand || monster.activeForTicks == 0) {
		return;
	}

	Direction md = GetDirection(monster.position.tile, monster.position.last);
	monster.direction = md;
	if (monster.distanceToEnemy() >= 2) {
		if (static_cast<MonsterMode>(monster.var1) == MonsterMode::Delay || (GenerateRnd(100) >= 35 - 4 * monster.intelligence)) {
			RandomWalk(monster, md);
		} else {
			AiDelay(monster, 15 - 2 * monster.intelligence + GenerateRnd(10));
		}
	} else {
		if (static_cast<MonsterMode>(monster.var1) == MonsterMode::Delay || (GenerateRnd(100) < 2 * monster.intelligence + 20)) {
			StartAttack(monster);
		} else {
			AiDelay(monster, 2 * (5 - monster.intelligence) + GenerateRnd(10));
		}
	}

	monster.checkStandAnimationIsLoaded(md);
}

void SkeletonBowAi(Monster &monster)
{
	if (monster.mode != MonsterMode::Stand || monster.activeForTicks == 0) {
		return;
	}

	Direction md = GetMonsterDirection(monster);
	monster.direction = md;
	int v = GenerateRnd(100);

	bool walking = false;

	if (monster.distanceToEnemy() < 4) {
		if ((monster.var2 > 20 && v < 2 * monster.intelligence + 13)
		    || (IsMonsterModeMove(static_cast<MonsterMode>(monster.var1))
		        && monster.var2 == 0
		        && v < 2 * monster.intelligence + 63)) {
			walking = Walk(monster, Opposite(md));
		}
	}

	if (!walking) {
		if (GenerateRnd(100) < 2 * monster.intelligence + 3) {
			if (LineClearMissile(monster.position.tile, monster.enemyPosition))
				StartRangedAttack(monster, MIS_ARROW, 4);
		}
	}

	monster.checkStandAnimationIsLoaded(md);
}

std::optional<Point> ScavengerFindCorpse(const Monster &scavenger)
{
	bool reverseSearch = FlipCoin();
	int first = reverseSearch ? 4 : -4;
	int last = reverseSearch ? -4 : 4;
	int increment = reverseSearch ? -1 : 1;

	for (int y = first; y <= last; y += increment) {
		for (int x = first; x <= last; x += increment) {
			Point position = scavenger.position.tile + Displacement { x, y };
			// BUGFIX: incorrect check of offset against limits of the dungeon (fixed)
			if (!InDungeonBounds(position))
				continue;
			if (dCorpse[position.x][position.y] == 0)
				continue;
			if (!IsLineNotSolid(scavenger.position.tile, position))
				continue;
			return position;
		}
	}
	return {};
}

void ScavengerAi(Monster &monster)
{
	if (monster.mode != MonsterMode::Stand)
		return;
	if (monster.hitPoints < (monster.maxHitPoints / 2) && monster.goal != MonsterGoal::Healing) {
		if (monster.leaderRelation != LeaderRelation::None) {
			ShrinkLeaderPacksize(monster);
			monster.leaderRelation = LeaderRelation::None;
		}
		monster.goal = MonsterGoal::Healing;
		monster.goalVar3 = 10;
	}
	if (monster.goal == MonsterGoal::Healing && monster.goalVar3 != 0) {
		monster.goalVar3--;
		if (dCorpse[monster.position.tile.x][monster.position.tile.y] != 0) {
			StartEating(monster);
			if ((monster.flags & MFLAG_NOHEAL) == 0) {
				if (gbIsHellfire) {
					int mMaxHP = monster.maxHitPoints; // BUGFIX use hitPointsMaximum or we loose health when difficulty isn't normal (fixed)
					monster.hitPoints += mMaxHP / 8;
					if (monster.hitPoints > monster.maxHitPoints)
						monster.hitPoints = monster.maxHitPoints;
					if (monster.goalVar3 <= 0 || monster.hitPoints == monster.maxHitPoints)
						dCorpse[monster.position.tile.x][monster.position.tile.y] = 0;
				} else {
					monster.hitPoints += 64;
				}
			}
			int targetHealth = monster.maxHitPoints;
			if (!gbIsHellfire)
				targetHealth = (monster.maxHitPoints / 2) + (monster.maxHitPoints / 4);
			if (monster.hitPoints >= targetHealth) {
				monster.goal = MonsterGoal::Normal;
				monster.goalVar1 = 0;
				monster.goalVar2 = 0;
			}
		} else {
			if (monster.goalVar1 == 0) {
				std::optional<Point> position = ScavengerFindCorpse(monster);
				if (position) {
					monster.goalVar1 = position->x + 1;
					monster.goalVar2 = position->y + 1;
				}
			}
			if (monster.goalVar1 != 0) {
				int x = monster.goalVar1 - 1;
				int y = monster.goalVar2 - 1;
				monster.direction = GetDirection(monster.position.tile, { x, y });
				RandomWalk(monster, monster.direction);
			}
		}
	}

	if (monster.mode == MonsterMode::Stand)
		SkeletonAi(monster);
}

void RhinoAi(Monster &monster)
{
	if (monster.mode != MonsterMode::Stand || monster.activeForTicks == 0) {
		return;
	}

	Direction md = GetDirection(monster.position.tile, monster.position.last);
	if (monster.activeForTicks < UINT8_MAX)
		MonstCheckDoors(monster);
	int v = GenerateRnd(100);
	unsigned distanceToEnemy = monster.distanceToEnemy();
	if (distanceToEnemy >= 2) {
		if (monster.goal == MonsterGoal::Move || (distanceToEnemy >= 5 && !FlipCoin(4))) {
			if (monster.goal != MonsterGoal::Move) {
				monster.goalVar1 = 0;
				monster.goalVar2 = GenerateRnd(2);
			}
			monster.goal = MonsterGoal::Move;
			if (monster.goalVar1++ >= static_cast<int>(2 * distanceToEnemy) || dTransVal[monster.position.tile.x][monster.position.tile.y] != dTransVal[monster.enemyPosition.x][monster.enemyPosition.y]) {
				monster.goal = MonsterGoal::Normal;
			} else if (!RoundWalk(monster, md, &monster.goalVar2)) {
				AiDelay(monster, GenerateRnd(10) + 10);
			}
		}
	} else {
		monster.goal = MonsterGoal::Normal;
	}
	if (monster.goal == MonsterGoal::Normal) {
		if (distanceToEnemy >= 5
		    && v < 2 * monster.intelligence + 43
		    && LineClear([&monster](Point position) { return IsTileAvailable(monster, position); }, monster.position.tile, monster.enemyPosition)) {
			size_t monsterId = monster.getId();
			if (AddMissile(monster.position.tile, monster.enemyPosition, md, MIS_RHINO, TARGET_PLAYERS, monsterId, 0, 0) != nullptr) {
				if (monster.data().hasSpecialSound)
					PlayEffect(monster, MonsterSound::Special);
				dMonster[monster.position.tile.x][monster.position.tile.y] = -(monsterId + 1);
				monster.mode = MonsterMode::Charge;
			}
		} else {
			if (distanceToEnemy >= 2) {
				v = GenerateRnd(100);
				if (v >= 2 * monster.intelligence + 33
				    && (IsNoneOf(static_cast<MonsterMode>(monster.var1), MonsterMode::MoveNorthwards, MonsterMode::MoveSouthwards, MonsterMode::MoveSideways)
				        || monster.var2 != 0
				        || v >= 2 * monster.intelligence + 83)) {
					AiDelay(monster, GenerateRnd(10) + 10);
				} else {
					RandomWalk(monster, md);
				}
			} else if (v < 2 * monster.intelligence + 28) {
				monster.direction = md;
				StartAttack(monster);
			}
		}
	}

	monster.checkStandAnimationIsLoaded(monster.direction);
}

void FallenAi(Monster &monster)
{
	if (monster.goal == MonsterGoal::Attack) {
		if (monster.goalVar1 != 0)
			monster.goalVar1--;
		else
			monster.goal = MonsterGoal::Normal;
	}
	if (monster.mode != MonsterMode::Stand || monster.activeForTicks == 0) {
		return;
	}

	if (monster.goal == MonsterGoal::Retreat) {
		if (monster.goalVar1-- == 0) {
			monster.goal = MonsterGoal::Normal;
			M_StartStand(monster, Opposite(static_cast<Direction>(monster.goalVar2)));
		}
	}

	if (monster.animInfo.isLastFrame()) {
		if (!FlipCoin(4)) {
			return;
		}
		if ((monster.flags & MFLAG_NOHEAL) == 0) {
			StartSpecialStand(monster, monster.direction);
			if (monster.maxHitPoints - (2 * monster.intelligence + 2) >= monster.hitPoints)
				monster.hitPoints += 2 * monster.intelligence + 2;
			else
				monster.hitPoints = monster.maxHitPoints;
		}
		int rad = 2 * monster.intelligence + 4;
		for (int y = -rad; y <= rad; y++) {
			for (int x = -rad; x <= rad; x++) {
				int xpos = monster.position.tile.x + x;
				int ypos = monster.position.tile.y + y;
				if (InDungeonBounds({ xpos, ypos })) {
					int m = dMonster[xpos][ypos];
					if (m <= 0)
						continue;

					auto &otherMonster = Monsters[m - 1];
					if (otherMonster.ai != AI_FALLEN)
						continue;

					otherMonster.goal = MonsterGoal::Attack;
					otherMonster.goalVar1 = 30 * monster.intelligence + 105;
				}
			}
		}
	} else if (monster.goal == MonsterGoal::Retreat) {
		monster.direction = static_cast<Direction>(monster.goalVar2);
		RandomWalk(monster, monster.direction);
	} else if (monster.goal == MonsterGoal::Attack) {
		if (monster.distanceToEnemy() < 2)
			StartAttack(monster);
		else
			RandomWalk(monster, GetMonsterDirection(monster));
	} else
		SkeletonAi(monster);
}

void LeoricAi(Monster &monster)
{
	if (monster.mode != MonsterMode::Stand || monster.activeForTicks == 0) {
		return;
	}

	Direction md = GetDirection(monster.position.tile, monster.position.last);
	if (monster.activeForTicks < UINT8_MAX)
		MonstCheckDoors(monster);
	int v = GenerateRnd(100);
	unsigned distanceToEnemy = monster.distanceToEnemy();
	if (distanceToEnemy >= 2 && monster.activeForTicks == UINT8_MAX && dTransVal[monster.position.tile.x][monster.position.tile.y] == dTransVal[monster.enemyPosition.x][monster.enemyPosition.y]) {
		if (monster.goal == MonsterGoal::Move || (distanceToEnemy >= 3 && FlipCoin(4))) {
			if (monster.goal != MonsterGoal::Move) {
				monster.goalVar1 = 0;
				monster.goalVar2 = GenerateRnd(2);
			}
			monster.goal = MonsterGoal::Move;
			if ((monster.goalVar1++ >= static_cast<int>(2 * distanceToEnemy) && DirOK(monster, md)) || dTransVal[monster.position.tile.x][monster.position.tile.y] != dTransVal[monster.enemyPosition.x][monster.enemyPosition.y]) {
				monster.goal = MonsterGoal::Normal;
			} else if (!RoundWalk(monster, md, &monster.goalVar2)) {
				AiDelay(monster, GenerateRnd(10) + 10);
			}
		}
	} else {
		monster.goal = MonsterGoal::Normal;
	}
	if (monster.goal == MonsterGoal::Normal) {
		if (!gbIsMultiplayer
		    && ((distanceToEnemy >= 3 && v < 4 * monster.intelligence + 35) || v < 6)
		    && LineClearMissile(monster.position.tile, monster.enemyPosition)) {
			Point newPosition = monster.position.tile + md;
			if (IsTileAvailable(monster, newPosition) && ActiveMonsterCount < MaxMonsters) {
				SpawnSkeleton(newPosition, md);
				StartSpecialStand(monster, md);
			}
		} else {
			if (distanceToEnemy >= 2) {
				v = GenerateRnd(100);
				if (v >= monster.intelligence + 25
				    && (IsNoneOf(static_cast<MonsterMode>(monster.var1), MonsterMode::MoveNorthwards, MonsterMode::MoveSouthwards, MonsterMode::MoveSideways) || monster.var2 != 0 || (v >= monster.intelligence + 75))) {
					AiDelay(monster, GenerateRnd(10) + 10);
				} else {
					RandomWalk(monster, md);
				}
			} else if (v < monster.intelligence + 20) {
				monster.direction = md;
				StartAttack(monster);
			}
		}
	}

	monster.checkStandAnimationIsLoaded(md);
}

void BatAi(Monster &monster)
{
	if (monster.mode != MonsterMode::Stand || monster.activeForTicks == 0) {
		return;
	}

	Direction md = GetDirection(monster.position.tile, monster.position.last);
	monster.direction = md;
	int v = GenerateRnd(100);
	if (monster.goal == MonsterGoal::Retreat) {
		if (monster.goalVar1 == 0) {
			RandomWalk(monster, Opposite(md));
			monster.goalVar1++;
		} else {
			RandomWalk(monster, PickRandomlyAmong({ Right(md), Left(md) }));
			monster.goal = MonsterGoal::Normal;
		}
		return;
	}

	unsigned distanceToEnemy = monster.distanceToEnemy();
	if (monster.type().type == MT_GLOOM
	    && distanceToEnemy >= 5
	    && v < 4 * monster.intelligence + 33
	    && LineClear([&monster](Point position) { return IsTileAvailable(monster, position); }, monster.position.tile, monster.enemyPosition)) {
		size_t monsterId = monster.getId();
		if (AddMissile(monster.position.tile, monster.enemyPosition, md, MIS_RHINO, TARGET_PLAYERS, monsterId, 0, 0) != nullptr) {
			dMonster[monster.position.tile.x][monster.position.tile.y] = -(monsterId + 1);
			monster.mode = MonsterMode::Charge;
		}
	} else if (distanceToEnemy >= 2) {
		if ((monster.var2 > 20 && v < monster.intelligence + 13)
		    || (IsMonsterModeMove(static_cast<MonsterMode>(monster.var1))
		        && monster.var2 == 0
		        && v < monster.intelligence + 63)) {
			RandomWalk(monster, md);
		}
	} else if (v < 4 * monster.intelligence + 8) {
		StartAttack(monster);
		monster.goal = MonsterGoal::Retreat;
		monster.goalVar1 = 0;
		if (monster.type().type == MT_FAMILIAR) {
			AddMissile(monster.enemyPosition, { monster.enemyPosition.x + 1, 0 }, Direction::South, MIS_LIGHTNING, TARGET_PLAYERS, monster.getId(), GenerateRnd(10) + 1, 0);
		}
	}

	monster.checkStandAnimationIsLoaded(md);
}

void GargoyleAi(Monster &monster)
{
	Direction md = GetMonsterDirection(monster);
	unsigned distanceToEnemy = monster.distanceToEnemy();
	if (monster.activeForTicks != 0 && (monster.flags & MFLAG_ALLOW_SPECIAL) != 0) {
		UpdateEnemy(monster);
		if (distanceToEnemy < monster.intelligence + 2u) {
			monster.flags &= ~MFLAG_ALLOW_SPECIAL;
		}
		return;
	}

	if (monster.mode != MonsterMode::Stand || monster.activeForTicks == 0) {
		return;
	}

	if (monster.hitPoints < (monster.maxHitPoints / 2))
		if ((monster.flags & MFLAG_NOHEAL) == 0)
			monster.goal = MonsterGoal::Retreat;
	if (monster.goal == MonsterGoal::Retreat) {
		if (distanceToEnemy >= monster.intelligence + 2u) {
			monster.goal = MonsterGoal::Normal;
			StartHeal(monster);
		} else if (!RandomWalk(monster, Opposite(md))) {
			monster.goal = MonsterGoal::Normal;
		}
	}
	AiAvoidance(monster);
}

void ButcherAi(Monster &monster)
{
	if (monster.mode != MonsterMode::Stand || monster.activeForTicks == 0) {
		return;
	}

	Direction md = GetDirection(monster.position.tile, monster.position.last);
	monster.direction = md;

	if (monster.distanceToEnemy() >= 2)
		RandomWalk(monster, md);
	else
		StartAttack(monster);

	monster.checkStandAnimationIsLoaded(md);
}

void SneakAi(Monster &monster)
{
	if (monster.mode != MonsterMode::Stand) {
		return;
	}
	if (dLight[monster.position.tile.x][monster.position.tile.y] == LightsMax) {
		return;
	}

	unsigned dist = 5 - monster.intelligence;
	unsigned distanceToEnemy = monster.distanceToEnemy();
	if (static_cast<MonsterMode>(monster.var1) == MonsterMode::HitRecovery) {
		monster.goal = MonsterGoal::Retreat;
		monster.goalVar1 = 0;
	} else if (distanceToEnemy >= dist + 3 || monster.goalVar1 > 8) {
		monster.goal = MonsterGoal::Normal;
		monster.goalVar1 = 0;
	}
	Direction md = GetMonsterDirection(monster);
	if (monster.goal == MonsterGoal::Retreat && (monster.flags & MFLAG_NO_ENEMY) == 0) {
		if ((monster.flags & MFLAG_TARGETS_MONSTER) != 0)
			md = GetDirection(monster.position.tile, Monsters[monster.enemy].position.tile);
		else
			md = GetDirection(monster.position.tile, Players[monster.enemy].position.last);
		md = Opposite(md);
		if (monster.type().type == MT_UNSEEN) {
			md = PickRandomlyAmong({ Right(md), Left(md) });
		}
	}
	monster.direction = md;
	int v = GenerateRnd(100);
	if (distanceToEnemy < dist && (monster.flags & MFLAG_HIDDEN) != 0) {
		StartFadein(monster, md, false);
	} else {
		if ((distanceToEnemy >= dist + 1) && (monster.flags & MFLAG_HIDDEN) == 0) {
			StartFadeout(monster, md, true);
		} else {
			if (monster.goal == MonsterGoal::Retreat
			    || (distanceToEnemy >= 2
			        && ((monster.var2 > 20 && v < 4 * monster.intelligence + 14)
			            || (IsMonsterModeMove(static_cast<MonsterMode>(monster.var1))
			                && monster.var2 == 0 && v < 4 * monster.intelligence + 64)))) {
				monster.goalVar1++;
				RandomWalk(monster, md);
			}
		}
	}
	if (monster.mode == MonsterMode::Stand) {
		if (distanceToEnemy >= 2 || v >= 4 * monster.intelligence + 10)
			monster.changeAnimationData(MonsterGraphic::Stand);
		else
			StartAttack(monster);
	}
}

void GharbadAi(Monster &monster)
{
	if (monster.mode != MonsterMode::Stand) {
		return;
	}

	Direction md = GetMonsterDirection(monster);

	if (monster.talkMsg >= TEXT_GARBUD1
	    && monster.talkMsg <= TEXT_GARBUD3
	    && !IsTileVisible(monster.position.tile)
	    && monster.goal == MonsterGoal::Talking) {
		monster.goal = MonsterGoal::Inquiring;
		switch (monster.talkMsg) {
		case TEXT_GARBUD1:
			monster.talkMsg = TEXT_GARBUD2;
			break;
		case TEXT_GARBUD2:
			monster.talkMsg = TEXT_GARBUD3;
			break;
		case TEXT_GARBUD3:
			monster.talkMsg = TEXT_GARBUD4;
			break;
		default:
			break;
		}
	}

	if (IsTileVisible(monster.position.tile)) {
		if (monster.talkMsg == TEXT_GARBUD4) {
			if (!effect_is_playing(USFX_GARBUD4) && monster.goal == MonsterGoal::Talking) {
				monster.goal = MonsterGoal::Normal;
				monster.activeForTicks = UINT8_MAX;
				monster.talkMsg = TEXT_NONE;
			}
		}
	}

	if (IsAnyOf(monster.goal, MonsterGoal::Normal, MonsterGoal::Move))
		AiAvoidance(monster);

	monster.checkStandAnimationIsLoaded(md);
}

void SnotSpilAi(Monster &monster)
{
	if (monster.mode != MonsterMode::Stand) {
		return;
	}

	Direction md = GetMonsterDirection(monster);

	if (monster.talkMsg == TEXT_BANNER10 && !IsTileVisible(monster.position.tile) && monster.goal == MonsterGoal::Talking) {
		monster.talkMsg = TEXT_BANNER11;
		monster.goal = MonsterGoal::Inquiring;
	}

	if (monster.talkMsg == TEXT_BANNER11 && Quests[Q_LTBANNER]._qvar1 == 3) {
		monster.talkMsg = TEXT_NONE;
		monster.goal = MonsterGoal::Normal;
	}

	if (IsTileVisible(monster.position.tile)) {
		if (monster.talkMsg == TEXT_BANNER12) {
			if (!effect_is_playing(USFX_SNOT3) && monster.goal == MonsterGoal::Talking) {
				ObjChangeMap(SetPiece.position.x, SetPiece.position.y, SetPiece.position.x + SetPiece.size.width + 1, SetPiece.position.y + SetPiece.size.height + 1);
				Quests[Q_LTBANNER]._qvar1 = 3;
				RedoPlayerVision();
				monster.activeForTicks = UINT8_MAX;
				monster.talkMsg = TEXT_NONE;
				monster.goal = MonsterGoal::Normal;
			}
		}
		if (Quests[Q_LTBANNER]._qvar1 == 3) {
			if (IsAnyOf(monster.goal, MonsterGoal::Normal, MonsterGoal::Attack))
				FallenAi(monster);
		}
	}

	monster.checkStandAnimationIsLoaded(md);
}

void SnakeAi(Monster &monster)
{
	char pattern[6] = { 1, 1, 0, -1, -1, 0 };
	if (monster.mode != MonsterMode::Stand || monster.activeForTicks == 0)
		return;
	Direction md = GetDirection(monster.position.tile, monster.position.last);
	monster.direction = md;
	unsigned distanceToEnemy = monster.distanceToEnemy();
	if (distanceToEnemy >= 2) {
		if (distanceToEnemy < 3 && LineClear([&monster](Point position) { return IsTileAvailable(monster, position); }, monster.position.tile, monster.enemyPosition) && static_cast<MonsterMode>(monster.var1) != MonsterMode::Charge) {
			size_t monsterId = monster.getId();
			if (AddMissile(monster.position.tile, monster.enemyPosition, md, MIS_RHINO, TARGET_PLAYERS, monsterId, 0, 0) != nullptr) {
				PlayEffect(monster, MonsterSound::Attack);
				dMonster[monster.position.tile.x][monster.position.tile.y] = -(monsterId + 1);
				monster.mode = MonsterMode::Charge;
			}
		} else if (static_cast<MonsterMode>(monster.var1) == MonsterMode::Delay || GenerateRnd(100) >= 35 - 2 * monster.intelligence) {
			if (pattern[monster.goalVar1] == -1)
				md = Left(md);
			else if (pattern[monster.goalVar1] == 1)
				md = Right(md);

			monster.goalVar1++;
			if (monster.goalVar1 > 5)
				monster.goalVar1 = 0;

			Direction targetDirection = static_cast<Direction>(monster.goalVar2);
			if (md != targetDirection) {
				int drift = static_cast<int>(md) - monster.goalVar2;
				if (drift < 0)
					drift += 8;

				if (drift < 4)
					md = Right(targetDirection);
				else if (drift > 4)
					md = Left(targetDirection);
				monster.goalVar2 = static_cast<int>(md);
			}

			if (!Walk(monster, md))
				RandomWalk2(monster, monster.direction);
		} else {
			AiDelay(monster, 15 - monster.intelligence + GenerateRnd(10));
		}
	} else {
		if (IsAnyOf(static_cast<MonsterMode>(monster.var1), MonsterMode::Delay, MonsterMode::Charge)
		    || (GenerateRnd(100) < monster.intelligence + 20)) {
			StartAttack(monster);
		} else
			AiDelay(monster, 10 - monster.intelligence + GenerateRnd(10));
	}

	monster.checkStandAnimationIsLoaded(monster.direction);
}

void CounselorAi(Monster &monster)
{
	if (monster.mode != MonsterMode::Stand || monster.activeForTicks == 0) {
		return;
	}
	Direction md = GetDirection(monster.position.tile, monster.position.last);
	if (monster.activeForTicks < UINT8_MAX)
		MonstCheckDoors(monster);
	int v = GenerateRnd(100);
	unsigned distanceToEnemy = monster.distanceToEnemy();
	if (monster.goal == MonsterGoal::Retreat) {
		if (monster.goalVar1++ <= 3)
			RandomWalk(monster, Opposite(md));
		else {
			monster.goal = MonsterGoal::Normal;
			StartFadein(monster, md, true);
		}
	} else if (monster.goal == MonsterGoal::Move) {
		if (distanceToEnemy >= 2 && monster.activeForTicks == UINT8_MAX && dTransVal[monster.position.tile.x][monster.position.tile.y] == dTransVal[monster.enemyPosition.x][monster.enemyPosition.y]) {
			if (monster.goalVar1++ < static_cast<int>(2 * distanceToEnemy) || !DirOK(monster, md)) {
				RoundWalk(monster, md, &monster.goalVar2);
			} else {
				monster.goal = MonsterGoal::Normal;
				StartFadein(monster, md, true);
			}
		} else {
			monster.goal = MonsterGoal::Normal;
			StartFadein(monster, md, true);
		}
	} else if (monster.goal == MonsterGoal::Normal) {
		if (distanceToEnemy >= 2) {
			if (v < 5 * (monster.intelligence + 10) && LineClearMissile(monster.position.tile, monster.enemyPosition)) {
				constexpr missile_id MissileTypes[4] = { MIS_FIREBOLT, MIS_CBOLT, MIS_LIGHTCTRL, MIS_FIREBALL };
				StartRangedAttack(monster, MissileTypes[monster.intelligence], monster.minDamage + GenerateRnd(monster.maxDamage - monster.minDamage + 1));
			} else if (GenerateRnd(100) < 30) {
				monster.goal = MonsterGoal::Move;
				monster.goalVar1 = 0;
				StartFadeout(monster, md, false);
			} else
				AiDelay(monster, GenerateRnd(10) + 2 * (5 - monster.intelligence));
		} else {
			monster.direction = md;
			if (monster.hitPoints < (monster.maxHitPoints / 2)) {
				monster.goal = MonsterGoal::Retreat;
				monster.goalVar1 = 0;
				StartFadeout(monster, md, false);
			} else if (static_cast<MonsterMode>(monster.var1) == MonsterMode::Delay
			    || GenerateRnd(100) < 2 * monster.intelligence + 20) {
				StartRangedAttack(monster, MIS_NULL, 0);
				size_t monsterId = monster.getId();
				AddMissile(monster.position.tile, { 0, 0 }, monster.direction, MIS_FLASH, TARGET_PLAYERS, monsterId, 4, 0);
				AddMissile(monster.position.tile, { 0, 0 }, monster.direction, MIS_FLASH2, TARGET_PLAYERS, monsterId, 4, 0);
			} else
				AiDelay(monster, GenerateRnd(10) + 2 * (5 - monster.intelligence));
		}
	}
	if (monster.mode == MonsterMode::Stand) {
		AiDelay(monster, GenerateRnd(10) + 5);
	}
}

void ZharAi(Monster &monster)
{
	if (monster.mode != MonsterMode::Stand) {
		return;
	}

	Direction md = GetMonsterDirection(monster);
	if (monster.talkMsg == TEXT_ZHAR1 && !IsTileVisible(monster.position.tile) && monster.goal == MonsterGoal::Talking) {
		monster.talkMsg = TEXT_ZHAR2;
		monster.goal = MonsterGoal::Inquiring;
	}

	if (IsTileVisible(monster.position.tile)) {
		if (monster.talkMsg == TEXT_ZHAR2) {
			if (!effect_is_playing(USFX_ZHAR2) && monster.goal == MonsterGoal::Talking) {
				monster.activeForTicks = UINT8_MAX;
				monster.talkMsg = TEXT_NONE;
				monster.goal = MonsterGoal::Normal;
			}
		}
	}

	if (IsAnyOf(monster.goal, MonsterGoal::Normal, MonsterGoal::Retreat, MonsterGoal::Move))
		CounselorAi(monster);

	monster.checkStandAnimationIsLoaded(md);
}

void MegaAi(Monster &monster)
{
	unsigned distanceToEnemy = monster.distanceToEnemy();
	if (distanceToEnemy >= 5) {
		SkeletonAi(monster);
		return;
	}

	if (monster.mode != MonsterMode::Stand || monster.activeForTicks == 0) {
		return;
	}

	Direction md = GetDirection(monster.position.tile, monster.position.last);
	if (monster.activeForTicks < UINT8_MAX)
		MonstCheckDoors(monster);
	int v = GenerateRnd(100);
	if (distanceToEnemy >= 2 && monster.activeForTicks == UINT8_MAX && dTransVal[monster.position.tile.x][monster.position.tile.y] == dTransVal[monster.enemyPosition.x][monster.enemyPosition.y]) {
		if (monster.goal == MonsterGoal::Move || distanceToEnemy >= 3) {
			if (monster.goal != MonsterGoal::Move) {
				monster.goalVar1 = 0;
				monster.goalVar2 = GenerateRnd(2);
			}
			monster.goal = MonsterGoal::Move;
			monster.goalVar3 = 4;
			if (monster.goalVar1++ < static_cast<int>(2 * distanceToEnemy) || !DirOK(monster, md)) {
				if (v < 5 * (monster.intelligence + 16))
					RoundWalk(monster, md, &monster.goalVar2);
			} else
				monster.goal = MonsterGoal::Normal;
		}
	} else {
		monster.goal = MonsterGoal::Normal;
	}
	if (monster.goal == MonsterGoal::Normal) {
		if (((distanceToEnemy >= 3 && v < 5 * (monster.intelligence + 2)) || v < 5 * (monster.intelligence + 1) || monster.goalVar3 == 4) && LineClearMissile(monster.position.tile, monster.enemyPosition)) {
			StartRangedSpecialAttack(monster, MIS_FLAMEC, 0);
		} else if (distanceToEnemy >= 2) {
			v = GenerateRnd(100);
			if (v < 2 * (5 * monster.intelligence + 25)
			    || (IsMonsterModeMove(static_cast<MonsterMode>(monster.var1))
			        && monster.var2 == 0
			        && v < 2 * (5 * monster.intelligence + 40))) {
				RandomWalk(monster, md);
			}
		} else {
			if (GenerateRnd(100) < 10 * (monster.intelligence + 4)) {
				monster.direction = md;
				if (FlipCoin())
					StartRangedSpecialAttack(monster, MIS_FLAMEC, 0);
				else
					StartAttack(monster);
			}
		}
		monster.goalVar3 = 1;
	}
	if (monster.mode == MonsterMode::Stand) {
		AiDelay(monster, GenerateRnd(10) + 5);
	}
}

void LazarusAi(Monster &monster)
{
	if (monster.mode != MonsterMode::Stand) {
		return;
	}

	Direction md = GetMonsterDirection(monster);
	if (IsTileVisible(monster.position.tile)) {
		if (!gbIsMultiplayer) {
			Player &myPlayer = *MyPlayer;
			if (monster.talkMsg == TEXT_VILE13 && monster.goal == MonsterGoal::Inquiring && myPlayer.position.tile == Point { 35, 46 }) {
				PlayInGameMovie("gendata\\fprst3.smk");
				monster.mode = MonsterMode::Talk;
				Quests[Q_BETRAYER]._qvar1 = 5;
			}

			if (monster.talkMsg == TEXT_VILE13 && !effect_is_playing(USFX_LAZ1) && monster.goal == MonsterGoal::Talking) {
				ObjChangeMap(1, 18, 20, 24);
				RedoPlayerVision();
				Quests[Q_BETRAYER]._qvar1 = 6;
				monster.goal = MonsterGoal::Normal;
				monster.activeForTicks = UINT8_MAX;
				monster.talkMsg = TEXT_NONE;
			}
		}

		if (gbIsMultiplayer && monster.talkMsg == TEXT_VILE13 && monster.goal == MonsterGoal::Inquiring && Quests[Q_BETRAYER]._qvar1 <= 3) {
			monster.mode = MonsterMode::Talk;
		}
	}

	if (IsAnyOf(monster.goal, MonsterGoal::Normal, MonsterGoal::Retreat, MonsterGoal::Move)) {
		if (!gbIsMultiplayer && Quests[Q_BETRAYER]._qvar1 == 4 && monster.talkMsg == TEXT_NONE) { // Fix save games affected by teleport bug
			ObjChangeMapResync(1, 18, 20, 24);
			RedoPlayerVision();
			Quests[Q_BETRAYER]._qvar1 = 6;
		}
		monster.talkMsg = TEXT_NONE;
		CounselorAi(monster);
	}

	monster.checkStandAnimationIsLoaded(md);
}

void LazarusMinionAi(Monster &monster)
{
	if (monster.mode != MonsterMode::Stand)
		return;

	Direction md = GetMonsterDirection(monster);

	if (IsTileVisible(monster.position.tile)) {
		if (!gbIsMultiplayer) {
			if (Quests[Q_BETRAYER]._qvar1 <= 5) {
				monster.goal = MonsterGoal::Inquiring;
			} else {
				monster.goal = MonsterGoal::Normal;
				monster.talkMsg = TEXT_NONE;
			}
		} else
			monster.goal = MonsterGoal::Normal;
	}
	if (monster.goal == MonsterGoal::Normal)
		AiRanged(monster);

	monster.checkStandAnimationIsLoaded(md);
}

void LachdananAi(Monster &monster)
{
	if (monster.mode != MonsterMode::Stand) {
		return;
	}

	Direction md = GetMonsterDirection(monster);

	if (monster.talkMsg == TEXT_VEIL9 && !IsTileVisible(monster.position.tile) && monster.goal == MonsterGoal::Talking) {
		monster.talkMsg = TEXT_VEIL10;
		monster.goal = MonsterGoal::Inquiring;
	}

	if (IsTileVisible(monster.position.tile)) {
		if (monster.talkMsg == TEXT_VEIL11) {
			if (!effect_is_playing(USFX_LACH3) && monster.goal == MonsterGoal::Talking) {
				monster.talkMsg = TEXT_NONE;
				Quests[Q_VEIL]._qactive = QUEST_DONE;
				MonsterDeath(monster, monster.direction, true);
			}
		}
	}

	monster.checkStandAnimationIsLoaded(md);
}

void WarlordAi(Monster &monster)
{
	if (monster.mode != MonsterMode::Stand) {
		return;
	}

	Direction md = GetMonsterDirection(monster);
	if (IsTileVisible(monster.position.tile)) {
		if (monster.talkMsg == TEXT_WARLRD9 && monster.goal == MonsterGoal::Inquiring)
			monster.mode = MonsterMode::Talk;
		if (monster.talkMsg == TEXT_WARLRD9 && !effect_is_playing(USFX_WARLRD1) && monster.goal == MonsterGoal::Talking) {
			monster.activeForTicks = UINT8_MAX;
			monster.talkMsg = TEXT_NONE;
			monster.goal = MonsterGoal::Normal;
		}
	}

	if (monster.goal == MonsterGoal::Normal)
		SkeletonAi(monster);

	monster.checkStandAnimationIsLoaded(md);
}

void HorkDemonAi(Monster &monster)
{
	if (monster.mode != MonsterMode::Stand || monster.activeForTicks == 0) {
		return;
	}

	Direction md = GetDirection(monster.position.tile, monster.position.last);

	if (monster.activeForTicks < 255) {
		MonstCheckDoors(monster);
	}

	int v = GenerateRnd(100);

	unsigned distanceToEnemy = monster.distanceToEnemy();
	if (distanceToEnemy < 2) {
		monster.goal = MonsterGoal::Normal;
	} else if (monster.goal == MonsterGoal::Move || (distanceToEnemy >= 5 && !FlipCoin(4))) {
		if (monster.goal != MonsterGoal::Move) {
			monster.goalVar1 = 0;
			monster.goalVar2 = GenerateRnd(2);
		}
		monster.goal = MonsterGoal::Move;
		if (monster.goalVar1++ >= static_cast<int>(2 * distanceToEnemy) || dTransVal[monster.position.tile.x][monster.position.tile.y] != dTransVal[monster.enemyPosition.x][monster.enemyPosition.y]) {
			monster.goal = MonsterGoal::Normal;
		} else if (!RoundWalk(monster, md, &monster.goalVar2)) {
			AiDelay(monster, GenerateRnd(10) + 10);
		}
	}

	if (monster.goal == MonsterGoal::Normal) {
		if ((distanceToEnemy >= 3) && v < 2 * monster.intelligence + 43) {
			Point position = monster.position.tile + monster.direction;
			if (IsTileAvailable(monster, position) && ActiveMonsterCount < MaxMonsters) {
				StartRangedSpecialAttack(monster, MIS_HORKDMN, 0);
			}
		} else if (distanceToEnemy < 2) {
			if (v < 2 * monster.intelligence + 28) {
				monster.direction = md;
				StartAttack(monster);
			}
		} else {
			v = GenerateRnd(100);
			if (v < 2 * monster.intelligence + 33
			    || (IsMonsterModeMove(static_cast<MonsterMode>(monster.var1))
			        && monster.var2 == 0
			        && v < 2 * monster.intelligence + 83)) {
				RandomWalk(monster, md);
			} else {
				AiDelay(monster, GenerateRnd(10) + 10);
			}
		}
	}

	monster.checkStandAnimationIsLoaded(monster.direction);
}

string_view GetMonsterTypeText(const MonsterData &monsterData)
{
	switch (monsterData.monsterClass) {
	case MonsterClass::Animal:
		return _("Animal");
	case MonsterClass::Demon:
		return _("Demon");
	case MonsterClass::Undead:
		return _("Undead");
	}

	app_fatal(StrCat("Unknown monsterClass ", static_cast<int>(monsterData.monsterClass)));
}

void ActivateSpawn(Monster &monster, Point position, Direction dir)
{
	dMonster[position.x][position.y] = monster.getId() + 1;
	monster.position.tile = position;
	monster.position.future = position;
	monster.position.old = position;
	StartSpecialStand(monster, dir);
}

/** Maps from monster AI ID to monster AI function. */
void (*AiProc[])(Monster &monster) = {
	/*AI_ZOMBIE   */ &ZombieAi,
	/*AI_FAT      */ &OverlordAi,
	/*AI_SKELSD   */ &SkeletonAi,
	/*AI_SKELBOW  */ &SkeletonBowAi,
	/*AI_SCAV     */ &ScavengerAi,
	/*AI_RHINO    */ &RhinoAi,
	/*AI_GOATMC   */ &AiAvoidance,
	/*AI_GOATBOW  */ &AiRanged,
	/*AI_FALLEN   */ &FallenAi,
	/*AI_MAGMA    */ &AiRangedAvoidance,
	/*AI_SKELKING */ &LeoricAi,
	/*AI_BAT      */ &BatAi,
	/*AI_GARG     */ &GargoyleAi,
	/*AI_CLEAVER  */ &ButcherAi,
	/*AI_SUCC     */ &AiRanged,
	/*AI_SNEAK    */ &SneakAi,
	/*AI_STORM    */ &AiRangedAvoidance,
	/*AI_FIREMAN  */ nullptr,
	/*AI_GARBUD   */ &GharbadAi,
	/*AI_ACID     */ &AiRangedAvoidance,
	/*AI_ACIDUNIQ */ &AiRanged,
	/*AI_GOLUM    */ &GolumAi,
	/*AI_ZHAR     */ &ZharAi,
	/*AI_SNOTSPIL */ &SnotSpilAi,
	/*AI_SNAKE    */ &SnakeAi,
	/*AI_COUNSLR  */ &CounselorAi,
	/*AI_MEGA     */ &MegaAi,
	/*AI_DIABLO   */ &AiRangedAvoidance,
	/*AI_LAZARUS  */ &LazarusAi,
	/*AI_LAZHELP  */ &LazarusMinionAi,
	/*AI_LACHDAN  */ &LachdananAi,
	/*AI_WARLORD  */ &WarlordAi,
	/*AI_FIREBAT  */ &AiRanged,
	/*AI_TORCHANT */ &AiRanged,
	/*AI_HORKDMN  */ &HorkDemonAi,
	/*AI_LICH     */ &AiRanged,
	/*AI_ARCHLICH */ &AiRanged,
	/*AI_PSYCHORB */ &AiRanged,
	/*AI_NECROMORB*/ &AiRanged,
	/*AI_BONEDEMON*/ &AiRangedAvoidance
};

bool IsRelativeMoveOK(const Monster &monster, Point position, Direction mdir)
{
	Point futurePosition = position + mdir;
	if (!InDungeonBounds(futurePosition) || !IsTileAvailable(monster, futurePosition))
		return false;
	if (mdir == Direction::East) {
		if (IsTileSolid(position + Direction::SouthEast))
			return false;
	} else if (mdir == Direction::West) {
		if (IsTileSolid(position + Direction::SouthWest))
			return false;
	} else if (mdir == Direction::North) {
		if (IsTileSolid(position + Direction::NorthEast) || IsTileSolid(position + Direction::NorthWest))
			return false;
	} else if (mdir == Direction::South)
		if (IsTileSolid(position + Direction::SouthWest) || IsTileSolid(position + Direction::SouthEast))
			return false;
	return true;
}

bool IsMonsterAvalible(const MonsterData &monsterData)
{
	if (monsterData.availability == MonsterAvailability::Never)
		return false;

	if (gbIsSpawn && monsterData.availability == MonsterAvailability::Retail)
		return false;

	return currlevel >= monsterData.minDunLvl && currlevel <= monsterData.maxDunLvl;
}

bool UpdateModeStance(Monster &monster)
{
	switch (monster.mode) {
	case MonsterMode::Stand:
		MonsterIdle(monster);
		return false;
	case MonsterMode::MoveNorthwards:
	case MonsterMode::MoveSouthwards:
	case MonsterMode::MoveSideways:
		return MonsterWalk(monster, monster.mode);
	case MonsterMode::MeleeAttack:
		return MonsterAttack(monster);
	case MonsterMode::HitRecovery:
		return MonsterGotHit(monster);
	case MonsterMode::Death:
		MonsterDeath(monster);
		return false;
	case MonsterMode::SpecialMeleeAttack:
		return MonsterSpecialAttack(monster);
	case MonsterMode::FadeIn:
		return MonsterFadein(monster);
	case MonsterMode::FadeOut:
		return MonsterFadeout(monster);
	case MonsterMode::RangedAttack:
		return MonsterRangedAttack(monster);
	case MonsterMode::SpecialStand:
		return MonsterSpecialStand(monster);
	case MonsterMode::SpecialRangedAttack:
		return MonsterRangedSpecialAttack(monster);
	case MonsterMode::Delay:
		return MonsterDelay(monster);
	case MonsterMode::Petrified:
		MonsterPetrified(monster);
		return false;
	case MonsterMode::Heal:
		MonsterHeal(monster);
		return false;
	case MonsterMode::Talk:
		MonsterTalk(monster);
		return false;
	default:
		return false;
	}
}

} // namespace

void InitTRNForUniqueMonster(Monster &monster)
{
	char filestr[64];
	*BufCopy(filestr, R"(monsters\monsters\)", UniqueMonstersData[static_cast<size_t>(monster.uniqueType)].mTrnName, ".trn") = '\0';
	monster.uniqueMonsterTRN = LoadFileInMem<uint8_t>(filestr);
}

void PrepareUniqueMonst(Monster &monster, UniqueMonsterType monsterType, size_t minionType, int bosspacksize, const UniqueMonsterData &uniqueMonsterData)
{
	monster.uniqueType = monsterType;
	monster.maxHitPoints = uniqueMonsterData.mmaxhp << 6;

	if (!gbIsMultiplayer)
		monster.maxHitPoints = std::max(monster.maxHitPoints / 2, 64);

	monster.hitPoints = monster.maxHitPoints;
	monster.ai = uniqueMonsterData.mAi;
	monster.intelligence = uniqueMonsterData.mint;
	monster.minDamage = uniqueMonsterData.mMinDamage;
	monster.maxDamage = uniqueMonsterData.mMaxDamage;
	monster.minDamageSpecial = uniqueMonsterData.mMinDamage;
	monster.maxDamageSpecial = uniqueMonsterData.mMaxDamage;
	monster.resistance = uniqueMonsterData.mMagicRes;
	monster.talkMsg = uniqueMonsterData.mtalkmsg;
	if (monsterType == UniqueMonsterType::HorkDemon)
		monster.lightId = NO_LIGHT; // BUGFIX monsters initial light id should be -1 (fixed)
	else
		monster.lightId = AddLight(monster.position.tile, 3);

	if (gbIsMultiplayer) {
		if (monster.ai == AI_LAZHELP)
			monster.talkMsg = TEXT_NONE;
		if (monster.ai == AI_LAZARUS && Quests[Q_BETRAYER]._qvar1 > 3) {
			monster.goal = MonsterGoal::Normal;
		} else if (monster.talkMsg != TEXT_NONE) {
			monster.goal = MonsterGoal::Inquiring;
		}
	} else if (monster.talkMsg != TEXT_NONE) {
		monster.goal = MonsterGoal::Inquiring;
	}

	if (sgGameInitInfo.nDifficulty == DIFF_NIGHTMARE) {
		monster.maxHitPoints = 3 * monster.maxHitPoints;
		if (gbIsHellfire)
			monster.maxHitPoints += (gbIsMultiplayer ? 100 : 50) << 6;
		else
			monster.maxHitPoints += 64;
		monster.hitPoints = monster.maxHitPoints;
		monster.minDamage = 2 * (monster.minDamage + 2);
		monster.maxDamage = 2 * (monster.maxDamage + 2);
		monster.minDamageSpecial = 2 * (monster.minDamageSpecial + 2);
		monster.maxDamageSpecial = 2 * (monster.maxDamageSpecial + 2);
	} else if (sgGameInitInfo.nDifficulty == DIFF_HELL) {
		monster.maxHitPoints = 4 * monster.maxHitPoints;
		if (gbIsHellfire)
			monster.maxHitPoints += (gbIsMultiplayer ? 200 : 100) << 6;
		else
			monster.maxHitPoints += 192;
		monster.hitPoints = monster.maxHitPoints;
		monster.minDamage = 4 * monster.minDamage + 6;
		monster.maxDamage = 4 * monster.maxDamage + 6;
		monster.minDamageSpecial = 4 * monster.minDamageSpecial + 6;
		monster.maxDamageSpecial = 4 * monster.maxDamageSpecial + 6;
	}

	InitTRNForUniqueMonster(monster);
	monster.uniqTrans = uniquetrans++;

	if (uniqueMonsterData.customToHit != 0) {
		monster.toHit = uniqueMonsterData.customToHit;

		if (sgGameInitInfo.nDifficulty == DIFF_NIGHTMARE) {
			monster.toHit += NightmareToHitBonus;
		} else if (sgGameInitInfo.nDifficulty == DIFF_HELL) {
			monster.toHit += HellToHitBonus;
		}
	}
	if (uniqueMonsterData.customArmorClass != 0) {
		monster.armorClass = uniqueMonsterData.customArmorClass;

		if (sgGameInitInfo.nDifficulty == DIFF_NIGHTMARE) {
			monster.armorClass += NightmareAcBonus;
		} else if (sgGameInitInfo.nDifficulty == DIFF_HELL) {
			monster.armorClass += HellAcBonus;
		}
	}

	if (uniqueMonsterData.monsterPack != UniqueMonsterPack::None) {
		PlaceGroup(minionType, bosspacksize, &monster, uniqueMonsterData.monsterPack == UniqueMonsterPack::Leashed);
	}

	if (monster.ai != AI_GARG) {
		monster.changeAnimationData(MonsterGraphic::Stand);
		monster.animInfo.currentFrame = GenerateRnd(monster.animInfo.numberOfFrames - 1);
		monster.flags &= ~MFLAG_ALLOW_SPECIAL;
		monster.mode = MonsterMode::Stand;
	}
}

void InitLevelMonsters()
{
	LevelMonsterTypeCount = 0;
	monstimgtot = 0;

	for (CMonster &levelMonsterType : LevelMonsterTypes) {
		levelMonsterType.placeFlags = 0;
	}

	ClrAllMonsters();
	ActiveMonsterCount = 0;
	totalmonsters = MaxMonsters;

	for (size_t i = 0; i < MaxMonsters; i++) {
		ActiveMonsters[i] = i;
	}

	uniquetrans = 0;
}

void GetLevelMTypes()
{
	AddMonsterType(MT_GOLEM, PLACE_SPECIAL);
	if (currlevel == 16) {
		AddMonsterType(MT_ADVOCATE, PLACE_SCATTER);
		AddMonsterType(MT_RBLACK, PLACE_SCATTER);
		AddMonsterType(MT_DIABLO, PLACE_SPECIAL);
		return;
	}

	if (currlevel == 18)
		AddMonsterType(MT_HORKSPWN, PLACE_SCATTER);
	if (currlevel == 19) {
		AddMonsterType(MT_HORKSPWN, PLACE_SCATTER);
		AddMonsterType(MT_HORKDMN, PLACE_UNIQUE);
	}
	if (currlevel == 20)
		AddMonsterType(MT_DEFILER, PLACE_UNIQUE);
	if (currlevel == 24) {
		AddMonsterType(MT_ARCHLICH, PLACE_SCATTER);
		AddMonsterType(MT_NAKRUL, PLACE_SPECIAL);
	}

	if (!setlevel) {
		if (Quests[Q_BUTCHER].IsAvailable())
			AddMonsterType(MT_CLEAVER, PLACE_SPECIAL);
		if (Quests[Q_GARBUD].IsAvailable())
			AddMonsterType(UniqueMonsterType::Garbud, PLACE_UNIQUE);
		if (Quests[Q_ZHAR].IsAvailable())
			AddMonsterType(UniqueMonsterType::Zhar, PLACE_UNIQUE);
		if (Quests[Q_LTBANNER].IsAvailable())
			AddMonsterType(UniqueMonsterType::SnotSpill, PLACE_UNIQUE);
		if (Quests[Q_VEIL].IsAvailable())
			AddMonsterType(UniqueMonsterType::Lachdan, PLACE_UNIQUE);
		if (Quests[Q_WARLORD].IsAvailable())
			AddMonsterType(UniqueMonsterType::WarlordOfBlood, PLACE_UNIQUE);

		if (gbIsMultiplayer && currlevel == Quests[Q_SKELKING]._qlevel) {

			AddMonsterType(MT_SKING, PLACE_UNIQUE);

			int skeletonTypeCount = 0;
			_monster_id skeltypes[NUM_MTYPES];
			for (_monster_id skeletonType : SkeletonTypes) {
				if (!IsMonsterAvalible(MonstersData[skeletonType]))
					continue;

				skeltypes[skeletonTypeCount++] = skeletonType;
			}
			AddMonsterType(skeltypes[GenerateRnd(skeletonTypeCount)], PLACE_SCATTER);
		}

		_monster_id typelist[MaxMonsters];

		int nt = 0;
		for (int i = MT_NZOMBIE; i < NUM_MTYPES; i++) {
			if (!IsMonsterAvalible(MonstersData[i]))
				continue;

			typelist[nt++] = (_monster_id)i;
		}

		while (nt > 0 && LevelMonsterTypeCount < MaxLvlMTypes && monstimgtot < 4000) {
			for (int i = 0; i < nt;) {
				if (MonstersData[typelist[i]].image > 4000 - monstimgtot) {
					typelist[i] = typelist[--nt];
					continue;
				}

				i++;
			}

			if (nt != 0) {
				int i = GenerateRnd(nt);
				AddMonsterType(typelist[i], PLACE_SCATTER);
				typelist[i] = typelist[--nt];
			}
		}
	} else {
		if (setlvlnum == SL_SKELKING) {
			AddMonsterType(MT_SKING, PLACE_UNIQUE);
		}
	}
}

void InitMonsterSND(CMonster &monsterType)
{
	if (!gbSndInited)
		return;

	const char *prefixes[] {
		"a", // Attack
		"h", // Hit
		"d", // Death
		"s", // Special
	};

	const MonsterData &data = MonstersData[monsterType.type];
	string_view soundSuffix = data.soundSuffix != nullptr ? data.soundSuffix : data.assetsSuffix;

	for (int i = 0; i < 4; i++) {
		string_view prefix = prefixes[i];
		if (prefix == "s" && !data.hasSpecialSound)
			continue;

		for (int j = 0; j < 2; j++) {
			char path[64];
			*BufCopy(path, "monsters\\", soundSuffix, prefix, j + 1, ".wav") = '\0';
			monsterType.sounds[i][j] = sound_file_load(path);
		}
	}
}

void InitMonsterGFX(CMonster &monsterType)
{
	const _monster_id mtype = monsterType.type;
	const MonsterData &monsterData = MonstersData[mtype];
	const size_t numAnims = GetNumAnims(monsterData);
	const auto hasAnim = [&monsterData](size_t index) {
		return monsterData.frames[index] != 0;
	};
	constexpr size_t MaxAnims = 6;
	std::array<uint32_t, MaxAnims + 1> animOffsets;
	if (!HeadlessMode) {
		monsterType.animData = MultiFileLoader<MaxAnims> {}(
		    numAnims,
		    FileNameWithCharAffixGenerator({ "monsters\\", monsterData.assetsSuffix }, DEVILUTIONX_CL2_EXT, Animletter),
		    animOffsets.data(),
		    hasAnim);
	}

	for (size_t i = 0, j = 0; i < numAnims; ++i) {
		AnimStruct &anim = monsterType.anims[i];
		if (!hasAnim(i)) {
			anim.frames = 0;
			continue;
		}
		anim.frames = monsterData.frames[i];
		anim.rate = monsterData.rate[i];
		anim.width = monsterData.width;
		if (!HeadlessMode) {
			const uint32_t begin = animOffsets[j];
			const uint32_t end = animOffsets[j + 1];
			auto spritesData = reinterpret_cast<uint8_t *>(&monsterType.animData[begin]);
			const uint16_t numLists = Cl2ToClx(spritesData, end - begin, PointerOrValue<uint16_t> { monsterData.width });
			anim.sprites = ClxSpriteListOrSheet { spritesData, numLists };
		}
		++j;
	}

	monsterType.data = &monsterData;

	if (HeadlessMode)
		return;

	if (monsterData.trnFile != nullptr) {
		InitMonsterTRN(monsterType);
	}

	if (IsAnyOf(mtype, MT_NMAGMA, MT_YMAGMA, MT_BMAGMA, MT_WMAGMA))
		MissileSpriteData[MFILE_MAGBALL].LoadGFX();
	if (IsAnyOf(mtype, MT_STORM, MT_RSTORM, MT_STORML, MT_MAEL))
		MissileSpriteData[MFILE_THINLGHT].LoadGFX();
	if (mtype == MT_SNOWWICH) {
		MissileSpriteData[MFILE_SCUBMISB].LoadGFX();
		MissileSpriteData[MFILE_SCBSEXPB].LoadGFX();
	}
	if (mtype == MT_HLSPWN) {
		MissileSpriteData[MFILE_SCUBMISD].LoadGFX();
		MissileSpriteData[MFILE_SCBSEXPD].LoadGFX();
	}
	if (mtype == MT_SOLBRNR) {
		MissileSpriteData[MFILE_SCUBMISC].LoadGFX();
		MissileSpriteData[MFILE_SCBSEXPC].LoadGFX();
	}
	if (IsAnyOf(mtype, MT_NACID, MT_RACID, MT_BACID, MT_XACID, MT_SPIDLORD)) {
		MissileSpriteData[MFILE_ACIDBF].LoadGFX();
		MissileSpriteData[MFILE_ACIDSPLA].LoadGFX();
		MissileSpriteData[MFILE_ACIDPUD].LoadGFX();
	}
	if (mtype == MT_LICH) {
		MissileSpriteData[MFILE_LICH].LoadGFX();
		MissileSpriteData[MFILE_EXORA1].LoadGFX();
	}
	if (mtype == MT_ARCHLICH) {
		MissileSpriteData[MFILE_ARCHLICH].LoadGFX();
		MissileSpriteData[MFILE_EXYEL2].LoadGFX();
	}
	if (IsAnyOf(mtype, MT_PSYCHORB, MT_BONEDEMN))
		MissileSpriteData[MFILE_BONEDEMON].LoadGFX();
	if (mtype == MT_NECRMORB) {
		MissileSpriteData[MFILE_NECROMORB].LoadGFX();
		MissileSpriteData[MFILE_EXRED3].LoadGFX();
	}
	if (mtype == MT_PSYCHORB)
		MissileSpriteData[MFILE_EXBL2].LoadGFX();
	if (mtype == MT_BONEDEMN)
		MissileSpriteData[MFILE_EXBL3].LoadGFX();
	if (mtype == MT_DIABLO)
		MissileSpriteData[MFILE_FIREPLAR].LoadGFX();
}

void WeakenNaKrul()
{
	if (currlevel != 24 || static_cast<size_t>(UberDiabloMonsterIndex) >= ActiveMonsterCount)
		return;

	auto &monster = Monsters[UberDiabloMonsterIndex];
	PlayEffect(monster, MonsterSound::Death);
	Quests[Q_NAKRUL]._qlog = false;
	monster.armorClass -= 50;
	int hp = monster.maxHitPoints / 2;
	monster.resistance = 0;
	monster.hitPoints = hp;
	monster.maxHitPoints = hp;
}

void InitGolems()
{
	if (!setlevel) {
		for (int i = 0; i < MAX_PLRS; i++)
			AddMonster(GolemHoldingCell, Direction::South, 0, false);
	}
}

void InitMonsters()
{
	if (!gbIsSpawn && !setlevel && currlevel == 16)
		LoadDiabMonsts();

	int nt = numtrigs;
	if (currlevel == 15)
		nt = 1;
	for (int i = 0; i < nt; i++) {
		for (int s = -2; s < 2; s++) {
			for (int t = -2; t < 2; t++)
				DoVision(trigs[i].position + Displacement { s, t }, 15, MAP_EXP_NONE, false);
		}
	}
	if (!gbIsSpawn)
		PlaceQuestMonsters();
	if (!setlevel) {
		if (!gbIsSpawn)
			PlaceUniqueMonsters();
		int na = 0;
		for (int s = 16; s < 96; s++) {
			for (int t = 16; t < 96; t++) {
				if (!IsTileSolid({ s, t }))
					na++;
			}
		}
		int numplacemonsters = na / 30;
		if (gbIsMultiplayer)
			numplacemonsters += numplacemonsters / 2;
		if (ActiveMonsterCount + numplacemonsters > MaxMonsters - 10)
			numplacemonsters = MaxMonsters - 10 - ActiveMonsterCount;
		totalmonsters = ActiveMonsterCount + numplacemonsters;
		int numscattypes = 0;
		size_t scattertypes[NUM_MTYPES];
		for (size_t i = 0; i < LevelMonsterTypeCount; i++) {
			if ((LevelMonsterTypes[i].placeFlags & PLACE_SCATTER) != 0) {
				scattertypes[numscattypes] = i;
				numscattypes++;
			}
		}
		while (ActiveMonsterCount < totalmonsters) {
			const size_t typeIndex = scattertypes[GenerateRnd(numscattypes)];
			if (currlevel == 1 || FlipCoin())
				na = 1;
			else if (currlevel == 2 || leveltype == DTYPE_CRYPT)
				na = GenerateRnd(2) + 2;
			else
				na = GenerateRnd(3) + 3;
			PlaceGroup(typeIndex, na);
		}
	}
	for (int i = 0; i < nt; i++) {
		for (int s = -2; s < 2; s++) {
			for (int t = -2; t < 2; t++)
				DoUnVision(trigs[i].position + Displacement { s, t }, 15);
		}
	}
}

void SetMapMonsters(const uint16_t *dunData, Point startPosition)
{
	AddMonsterType(MT_GOLEM, PLACE_SPECIAL);
	if (setlevel)
		for (int i = 0; i < MAX_PLRS; i++)
			AddMonster(GolemHoldingCell, Direction::South, 0, false);

	if (setlevel && setlvlnum == SL_VILEBETRAYER) {
		AddMonsterType(UniqueMonsterType::Lazarus, PLACE_UNIQUE);
		AddMonsterType(UniqueMonsterType::RedVex, PLACE_UNIQUE);
		AddMonsterType(UniqueMonsterType::BlackJade, PLACE_UNIQUE);
		PlaceUniqueMonst(UniqueMonsterType::Lazarus, 0, 0);
		PlaceUniqueMonst(UniqueMonsterType::RedVex, 0, 0);
		PlaceUniqueMonst(UniqueMonsterType::BlackJade, 0, 0);
	}

	int width = SDL_SwapLE16(dunData[0]);
	int height = SDL_SwapLE16(dunData[1]);

	int layer2Offset = 2 + width * height;

	// The rest of the layers are at dPiece scale
	width *= 2;
	height *= 2;

	const uint16_t *monsterLayer = &dunData[layer2Offset + width * height];

	for (int j = 0; j < height; j++) {
		for (int i = 0; i < width; i++) {
			auto monsterId = static_cast<uint8_t>(SDL_SwapLE16(monsterLayer[j * width + i]));
			if (monsterId != 0) {
				const size_t typeIndex = AddMonsterType(MonstConvTbl[monsterId - 1], PLACE_SPECIAL);
				PlaceMonster(ActiveMonsterCount++, typeIndex, startPosition + Displacement { i, j });
			}
		}
	}
}

Monster *AddMonster(Point position, Direction dir, size_t typeIndex, bool inMap)
{
	if (ActiveMonsterCount < MaxMonsters) {
		Monster &monster = Monsters[ActiveMonsters[ActiveMonsterCount++]];
		if (inMap)
			dMonster[position.x][position.y] = monster.getId() + 1;
		InitMonster(monster, dir, typeIndex, position);
		return &monster;
	}

	return nullptr;
}

void AddDoppelganger(Monster &monster)
{
	Point target = { 0, 0 };
	for (int d = 0; d < 8; d++) {
		const Point position = monster.position.tile + static_cast<Direction>(d);
		if (!IsTileAvailable(position))
			continue;
		target = position;
	}
	if (target != Point { 0, 0 }) {
		const size_t typeIndex = GetMonsterTypeIndex(monster.type().type);
		AddMonster(target, monster.direction, typeIndex, true);
	}
}

void ApplyMonsterDamage(Monster &monster, int damage)
{
	monster.hitPoints -= damage;

	if (monster.hitPoints >> 6 <= 0) {
		delta_kill_monster(monster, monster.position.tile, *MyPlayer);
		NetSendCmdLocParam1(false, CMD_MONSTDEATH, monster.position.tile, monster.getId());
		return;
	}

	delta_monster_hp(monster, *MyPlayer);
	NetSendCmdMonDmg(false, monster.getId(), damage);
}

bool M_Talker(const Monster &monster)
{
	return IsAnyOf(monster.ai, AI_LAZARUS, AI_WARLORD, AI_GARBUD, AI_ZHAR, AI_SNOTSPIL, AI_LACHDAN, AI_LAZHELP);
}

void M_StartStand(Monster &monster, Direction md)
{
	ClearMVars(monster);
	if (monster.type().type == MT_GOLEM)
		NewMonsterAnim(monster, MonsterGraphic::Walk, md);
	else
		NewMonsterAnim(monster, MonsterGraphic::Stand, md);
	monster.var1 = static_cast<int>(monster.mode);
	monster.var2 = 0;
	monster.mode = MonsterMode::Stand;
	monster.position.future = monster.position.tile;
	monster.position.old = monster.position.tile;
	UpdateEnemy(monster);
}

void M_ClearSquares(const Monster &monster)
{
	for (Point searchTile : PointsInRectangle(Rectangle { monster.position.old, 1 })) {
		if (FindMonsterAtPosition(searchTile) == &monster)
			dMonster[searchTile.x][searchTile.y] = 0;
	}
}

void M_GetKnockback(Monster &monster)
{
	Direction dir = Opposite(monster.direction);
	if (!IsRelativeMoveOK(monster, monster.position.old, dir)) {
		return;
	}

	M_ClearSquares(monster);
	monster.position.old += dir;
	StartMonsterGotHit(monster);
}

void M_StartHit(Monster &monster, int dam)
{
	PlayEffect(monster, MonsterSound::Hit);

	if (IsHardHit(monster, dam)) {
		if (monster.type().type == MT_BLINK) {
			Teleport(monster);
		} else if (IsAnyOf(monster.type().type, MT_NSCAV, MT_BSCAV, MT_WSCAV, MT_YSCAV, MT_GRAVEDIG)) {
			monster.goal = MonsterGoal::Normal;
			monster.goalVar1 = 0;
			monster.goalVar2 = 0;
		}
		if (monster.mode != MonsterMode::Petrified) {
			StartMonsterGotHit(monster);
		}
	}
}

void M_StartHit(Monster &monster, const Player &player, int dam)
{
	monster.tag(player);
	if (IsHardHit(monster, dam)) {
		monster.enemy = player.getId();
		monster.enemyPosition = player.position.future;
		monster.flags &= ~MFLAG_TARGETS_MONSTER;
		monster.direction = GetMonsterDirection(monster);
	}

	M_StartHit(monster, dam);
}

void MonsterDeath(Monster &monster, Direction md, bool sendmsg)
{
	if (!monster.isPlayerMinion())
		AddPlrMonstExper(monster.level(sgGameInitInfo.nDifficulty), monster.exp(sgGameInitInfo.nDifficulty), monster.whoHit);

	MonsterKillCounts[monster.type().type]++;
	monster.hitPoints = 0;
	monster.flags &= ~MFLAG_HIDDEN;
	SetRndSeed(monster.rndItemSeed);

	SpawnLoot(monster, sendmsg);

	if (monster.type().type == MT_DIABLO)
		DiabloDeath(monster, true);
	else
		PlayEffect(monster, MonsterSound::Death);

	if (monster.mode != MonsterMode::Petrified) {
		if (monster.type().type == MT_GOLEM)
			md = Direction::South;
		NewMonsterAnim(monster, MonsterGraphic::Death, md, gGameLogicStep < GameLogicStep::ProcessMonsters ? AnimationDistributionFlags::ProcessAnimationPending : AnimationDistributionFlags::None);
		monster.mode = MonsterMode::Death;
	}
	monster.goal = MonsterGoal::None;
	monster.var1 = 0;
	monster.position.tile = monster.position.old;
	monster.position.future = monster.position.old;
	M_ClearSquares(monster);
	dMonster[monster.position.tile.x][monster.position.tile.y] = monster.getId() + 1;
	CheckQuestKill(monster, sendmsg);
	M_FallenFear(monster.position.tile);
	if (IsAnyOf(monster.type().type, MT_NACID, MT_RACID, MT_BACID, MT_XACID, MT_SPIDLORD))
		AddMissile(monster.position.tile, { 0, 0 }, Direction::South, MIS_ACIDPUD, TARGET_PLAYERS, monster.getId(), monster.intelligence + 1, 0);
}

void StartMonsterDeath(Monster &monster, const Player &player, bool sendmsg)
{
	monster.tag(player);
	Direction md = GetDirection(monster.position.tile, player.position.tile);
	MonsterDeath(monster, md, sendmsg);
}

void KillMyGolem()
{
	Monster &golem = Monsters[MyPlayerId];
	delta_kill_monster(golem, golem.position.tile, *MyPlayer);
	NetSendCmdLoc(MyPlayerId, false, CMD_KILLGOLEM, golem.position.tile);
	M_StartKill(golem, *MyPlayer);
}

void M_StartKill(Monster &monster, const Player &player)
{
	StartMonsterDeath(monster, player, true);
}

void M_SyncStartKill(Monster &monster, Point position, const Player &player)
{
	if (monster.hitPoints == 0 || monster.mode == MonsterMode::Death) {
		return;
	}

	if (dMonster[position.x][position.y] == 0) {
		M_ClearSquares(monster);
		monster.position.tile = position;
		monster.position.old = position;
	}

	StartMonsterDeath(monster, player, false);
}

void M_UpdateRelations(const Monster &monster)
{
	if (monster.hasLeashedMinions())
		ReleaseMinions(monster);

	ShrinkLeaderPacksize(monster);
}

void DoEnding()
{
	if (gbIsMultiplayer) {
		SNetLeaveGame(LEAVE_ENDING);
	}

	music_stop();

	if (gbIsMultiplayer) {
		SDL_Delay(1000);
	}

	if (gbIsSpawn)
		return;

	switch (MyPlayer->_pClass) {
	case HeroClass::Sorcerer:
	case HeroClass::Monk:
		play_movie("gendata\\diabvic1.smk", false);
		break;
	case HeroClass::Warrior:
	case HeroClass::Barbarian:
		play_movie("gendata\\diabvic2.smk", false);
		break;
	default:
		play_movie("gendata\\diabvic3.smk", false);
		break;
	}
	play_movie("gendata\\diabend.smk", false);

	bool bMusicOn = gbMusicOn;
	gbMusicOn = true;

	int musicVolume = sound_get_or_set_music_volume(1);
	sound_get_or_set_music_volume(0);

	music_start(TMUSIC_CATACOMBS);
	loop_movie = true;
	play_movie("gendata\\loopdend.smk", true);
	loop_movie = false;
	music_stop();

	sound_get_or_set_music_volume(musicVolume);
	gbMusicOn = bMusicOn;
}

void PrepDoEnding()
{
	gbSoundOn = sgbSaveSoundOn;
	gbRunGame = false;
	MyPlayerIsDead = false;
	cineflag = true;

	Player &myPlayer = *MyPlayer;

	myPlayer.pDiabloKillLevel = std::max(myPlayer.pDiabloKillLevel, static_cast<uint8_t>(sgGameInitInfo.nDifficulty + 1));

	for (Player &player : Players) {
		player._pmode = PM_QUIT;
		player._pInvincible = true;
		if (gbIsMultiplayer) {
			if (player._pHitPoints >> 6 == 0)
				player._pHitPoints = 64;
			if (player._pMana >> 6 == 0)
				player._pMana = 64;
		}
	}
}

bool Walk(Monster &monster, Direction md)
{
	if (!DirOK(monster, md)) {
		return false;
	}

	switch (md) {
	case Direction::North:
		WalkNorthwards(monster, -1, -1, Direction::North);
		break;
	case Direction::NorthEast:
		WalkNorthwards(monster, 0, -1, Direction::NorthEast);
		break;
	case Direction::East:
		WalkSideways(monster, -32, -16, 1, -1, 1, 0, Direction::East);
		break;
	case Direction::SouthEast:
		WalkSouthwards(monster, -32, -16, 1, 0, Direction::SouthEast);
		break;
	case Direction::South:
		WalkSouthwards(monster, 0, -32, 1, 1, Direction::South);
		break;
	case Direction::SouthWest:
		WalkSouthwards(monster, 32, -16, 0, 1, Direction::SouthWest);
		break;
	case Direction::West:
		WalkSideways(monster, 32, -16, -1, 1, 0, 1, Direction::West);
		break;
	case Direction::NorthWest:
		WalkNorthwards(monster, -1, 0, Direction::NorthWest);
		break;
	case Direction::NoDirection:
		break;
	}
	return true;
}

void GolumAi(Monster &golem)
{
	if (golem.position.tile.x == 1 && golem.position.tile.y == 0) {
		return;
	}

	if (IsAnyOf(golem.mode, MonsterMode::Death, MonsterMode::SpecialStand) || golem.isWalking()) {
		return;
	}

	if ((golem.flags & MFLAG_TARGETS_MONSTER) == 0)
		UpdateEnemy(golem);

	if (golem.mode == MonsterMode::MeleeAttack) {
		return;
	}

	if ((golem.flags & MFLAG_NO_ENEMY) == 0) {
		auto &enemy = Monsters[golem.enemy];
		int mex = golem.position.tile.x - enemy.position.future.x;
		int mey = golem.position.tile.y - enemy.position.future.y;
		golem.direction = GetDirection(golem.position.tile, enemy.position.tile);
		if (abs(mex) < 2 && abs(mey) < 2) {
			golem.enemyPosition = enemy.position.tile;
			if (enemy.activeForTicks == 0) {
				enemy.activeForTicks = UINT8_MAX;
				enemy.position.last = golem.position.tile;
				for (int j = 0; j < 5; j++) {
					for (int k = 0; k < 5; k++) {
						int enemyId = dMonster[golem.position.tile.x + k - 2][golem.position.tile.y + j - 2]; // BUGFIX: Check if indexes are between 0 and 112
						if (enemyId > 0)
							Monsters[enemyId - 1].activeForTicks = UINT8_MAX; // BUGFIX: should be `Monsters[enemy-1]`, not Monsters[enemy]. (fixed)
					}
				}
			}
			StartAttack(golem);
			return;
		}
		if (AiPlanPath(golem))
			return;
	}

	golem.pathCount++;
	if (golem.pathCount > 8)
		golem.pathCount = 5;

	if (RandomWalk(golem, Players[golem.getId()]._pdir))
		return;

	Direction md = Left(golem.direction);
	for (int j = 0; j < 8; j++) {
		md = Right(md);
		if (Walk(golem, md)) {
			break;
		}
	}
}

void DeleteMonsterList()
{
	for (int i = 0; i < MAX_PLRS; i++) {
		auto &golem = Monsters[i];
		if (!golem.isInvalid)
			continue;

		golem.position.tile = GolemHoldingCell;
		golem.position.future = { 0, 0 };
		golem.position.old = { 0, 0 };
		golem.isInvalid = false;
	}

	for (size_t i = MAX_PLRS; i < ActiveMonsterCount;) {
		if (Monsters[ActiveMonsters[i]].isInvalid) {
			if (pcursmonst == ActiveMonsters[i]) // Unselect monster if player highlighted it
				pcursmonst = -1;
			DeleteMonster(i);
		} else {
			i++;
		}
	}
}

void ProcessMonsters()
{
	DeleteMonsterList();

	assert(ActiveMonsterCount <= MaxMonsters);
	for (size_t i = 0; i < ActiveMonsterCount; i++) {
		Monster &monster = Monsters[ActiveMonsters[i]];
		FollowTheLeader(monster);
		if (gbIsMultiplayer) {
			SetRndSeed(monster.aiSeed);
			monster.aiSeed = AdvanceRndSeed();
		}
		if ((monster.flags & MFLAG_NOHEAL) == 0 && monster.hitPoints < monster.maxHitPoints && monster.hitPoints >> 6 > 0) {
			if (monster.level(sgGameInitInfo.nDifficulty) > 1) {
				monster.hitPoints += monster.level(sgGameInitInfo.nDifficulty) / 2;
			} else {
				monster.hitPoints += monster.level(sgGameInitInfo.nDifficulty);
			}
			monster.hitPoints = std::min(monster.hitPoints, monster.maxHitPoints); // prevent going over max HP with part of a single regen tick
		}

		if (IsTileVisible(monster.position.tile) && monster.activeForTicks == 0) {
			if (monster.type().type == MT_CLEAVER) {
				PlaySFX(USFX_CLEAVER);
			}
			if (monster.type().type == MT_NAKRUL) {
				if (sgGameInitInfo.bCowQuest != 0) {
					PlaySFX(USFX_NAKRUL6);
				} else {
					if (IsUberRoomOpened)
						PlaySFX(USFX_NAKRUL4);
					else
						PlaySFX(USFX_NAKRUL5);
				}
			}
			if (monster.type().type == MT_DEFILER)
				PlaySFX(USFX_DEFILER8);
			UpdateEnemy(monster);
		}

		if ((monster.flags & MFLAG_TARGETS_MONSTER) != 0) {
			assert(monster.enemy >= 0 && monster.enemy < MaxMonsters);
			// BUGFIX: enemy target may be dead at time of access, thus reading garbage data from `Monsters[monster.enemy].position.future`.
			monster.position.last = Monsters[monster.enemy].position.future;
			monster.enemyPosition = monster.position.last;
		} else {
			assert(monster.enemy >= 0 && monster.enemy < MAX_PLRS);
			Player &player = Players[monster.enemy];
			monster.enemyPosition = player.position.future;
			if (IsTileVisible(monster.position.tile)) {
				monster.activeForTicks = UINT8_MAX;
				monster.position.last = player.position.future;
			} else if (monster.activeForTicks != 0 && monster.type().type != MT_DIABLO) {
				monster.activeForTicks--;
			}
		}
		while (true) {
			if ((monster.flags & MFLAG_SEARCH) == 0 || !AiPlanPath(monster)) {
				AiProc[monster.ai](monster);
			}

			if (!UpdateModeStance(monster))
				break;

			GroupUnity(monster);
		}
		if (monster.mode != MonsterMode::Petrified && (monster.flags & MFLAG_ALLOW_SPECIAL) == 0) {
			monster.animInfo.processAnimation((monster.flags & MFLAG_LOCK_ANIMATION) != 0);
		}
	}

	DeleteMonsterList();
}

void FreeMonsters()
{
	for (CMonster &monsterType : LevelMonsterTypes) {
		monsterType.animData = nullptr;
		for (AnimStruct &animData : monsterType.anims) {
			animData.sprites = std::nullopt;
		}

		for (auto &variants : monsterType.sounds) {
			for (auto &sound : variants) {
				sound = nullptr;
			}
		}
	}
}

bool DirOK(const Monster &monster, Direction mdir)
{
	Point position = monster.position.tile;
	Point futurePosition = position + mdir;
	if (!IsRelativeMoveOK(monster, position, mdir))
		return false;
	if (monster.leaderRelation == LeaderRelation::Leashed) {
		return futurePosition.WalkingDistance(monster.getLeader()->position.future) < 4;
	}
	if (!monster.hasLeashedMinions())
		return true;
	int mcount = 0;
	for (int x = futurePosition.x - 3; x <= futurePosition.x + 3; x++) {
		for (int y = futurePosition.y - 3; y <= futurePosition.y + 3; y++) {
			if (!InDungeonBounds({ x, y }))
				continue;
			Monster *minion = FindMonsterAtPosition({ x, y }, true);
			if (minion == nullptr)
				continue;

			if (minion->leaderRelation == LeaderRelation::Leashed && minion->getLeader() == &monster) {
				mcount++;
			}
		}
	}
	return mcount == monster.packSize;
}

bool PosOkMissile(Point position)
{
	return !TileHasAny(dPiece[position.x][position.y], TileProperties::BlockMissile);
}

bool LineClearMissile(Point startPoint, Point endPoint)
{
	return LineClear(PosOkMissile, startPoint, endPoint);
}

bool LineClear(tl::function_ref<bool(Point)> clear, Point startPoint, Point endPoint)
{
	Point position = startPoint;

	int dx = endPoint.x - position.x;
	int dy = endPoint.y - position.y;
	if (abs(dx) > abs(dy)) {
		if (dx < 0) {
			std::swap(position, endPoint);
			dx = -dx;
			dy = -dy;
		}
		int d;
		int yincD;
		int dincD;
		int dincH;
		if (dy > 0) {
			d = 2 * dy - dx;
			dincD = 2 * dy;
			dincH = 2 * (dy - dx);
			yincD = 1;
		} else {
			d = 2 * dy + dx;
			dincD = 2 * dy;
			dincH = 2 * (dx + dy);
			yincD = -1;
		}
		bool done = false;
		while (!done && position != endPoint) {
			if ((d <= 0) ^ (yincD < 0)) {
				d += dincD;
			} else {
				d += dincH;
				position.y += yincD;
			}
			position.x++;
			done = position != startPoint && !clear(position);
		}
	} else {
		if (dy < 0) {
			std::swap(position, endPoint);
			dy = -dy;
			dx = -dx;
		}
		int d;
		int xincD;
		int dincD;
		int dincH;
		if (dx > 0) {
			d = 2 * dx - dy;
			dincD = 2 * dx;
			dincH = 2 * (dx - dy);
			xincD = 1;
		} else {
			d = 2 * dx + dy;
			dincD = 2 * dx;
			dincH = 2 * (dy + dx);
			xincD = -1;
		}
		bool done = false;
		while (!done && position != endPoint) {
			if ((d <= 0) ^ (xincD < 0)) {
				d += dincD;
			} else {
				d += dincH;
				position.x += xincD;
			}
			position.y++;
			done = position != startPoint && !clear(position);
		}
	}
	return position == endPoint;
}

void SyncMonsterAnim(Monster &monster)
{
#ifdef _DEBUG
	// fix for saves with debug monsters having type originally not on the level
	CMonster &monsterType = LevelMonsterTypes[monster.levelType];
	if (monsterType.data == nullptr) {
		InitMonsterGFX(monsterType);
		monsterType.corpseId = 1;
	}
#endif
	if (monster.isUnique()) {
		InitTRNForUniqueMonster(monster);
	}
	MonsterGraphic graphic = MonsterGraphic::Stand;

	switch (monster.mode) {
	case MonsterMode::Stand:
	case MonsterMode::Delay:
	case MonsterMode::Talk:
		break;
	case MonsterMode::MoveNorthwards:
	case MonsterMode::MoveSouthwards:
	case MonsterMode::MoveSideways:
		graphic = MonsterGraphic::Walk;
		break;
	case MonsterMode::MeleeAttack:
	case MonsterMode::RangedAttack:
		graphic = MonsterGraphic::Attack;
		break;
	case MonsterMode::HitRecovery:
		graphic = MonsterGraphic::GotHit;
		break;
	case MonsterMode::Death:
		graphic = MonsterGraphic::Death;
		break;
	case MonsterMode::SpecialMeleeAttack:
	case MonsterMode::FadeIn:
	case MonsterMode::FadeOut:
	case MonsterMode::SpecialStand:
	case MonsterMode::SpecialRangedAttack:
	case MonsterMode::Heal:
		graphic = MonsterGraphic::Special;
		break;
	case MonsterMode::Charge:
		graphic = MonsterGraphic::Attack;
		monster.animInfo.currentFrame = 0;
		break;
	default:
		monster.animInfo.currentFrame = 0;
		break;
	}

	monster.changeAnimationData(graphic);
}

void M_FallenFear(Point position)
{
	const Rectangle fearArea = Rectangle { position, 4 };
	for (const Point tile : PointsInRectangle(fearArea)) {
		if (!InDungeonBounds(tile))
			continue;
		int m = dMonster[tile.x][tile.y];
		if (m == 0)
			continue;
		Monster &monster = Monsters[abs(m) - 1];
		if (monster.ai != AI_FALLEN || monster.hitPoints >> 6 <= 0)
			continue;

		int runDistance = std::max((8 - monster.data().level), 2);
		monster.goal = MonsterGoal::Retreat;
		monster.goalVar1 = runDistance;
		monster.goalVar2 = static_cast<int>(GetDirection(position, monster.position.tile));
	}
}

void PrintMonstHistory(int mt)
{
	if (*sgOptions.Gameplay.showMonsterType) {
		AddPanelString(fmt::format(fmt::runtime(_("Type: {:s}  Kills: {:d}")), GetMonsterTypeText(MonstersData[mt]), MonsterKillCounts[mt]));
	} else {
		AddPanelString(fmt::format(fmt::runtime(_("Total kills: {:d}")), MonsterKillCounts[mt]));
	}

	if (MonsterKillCounts[mt] >= 30) {
		int minHP = MonstersData[mt].hitPointsMinimum;
		int maxHP = MonstersData[mt].hitPointsMaximum;
		if (!gbIsHellfire && mt == MT_DIABLO) {
			minHP /= 2;
			maxHP /= 2;
		}
		if (!gbIsMultiplayer) {
			minHP /= 2;
			maxHP /= 2;
		}
		if (minHP < 1)
			minHP = 1;
		if (maxHP < 1)
			maxHP = 1;

		int hpBonusNightmare = 1;
		int hpBonusHell = 3;
		if (gbIsHellfire) {
			hpBonusNightmare = (!gbIsMultiplayer ? 50 : 100);
			hpBonusHell = (!gbIsMultiplayer ? 100 : 200);
		}
		if (sgGameInitInfo.nDifficulty == DIFF_NIGHTMARE) {
			minHP = 3 * minHP + hpBonusNightmare;
			maxHP = 3 * maxHP + hpBonusNightmare;
		} else if (sgGameInitInfo.nDifficulty == DIFF_HELL) {
			minHP = 4 * minHP + hpBonusHell;
			maxHP = 4 * maxHP + hpBonusHell;
		}
		AddPanelString(fmt::format(fmt::runtime(_("Hit Points: {:d}-{:d}")), minHP, maxHP));
	}
	if (MonsterKillCounts[mt] >= 15) {
		int res = (sgGameInitInfo.nDifficulty != DIFF_HELL) ? MonstersData[mt].resistance : MonstersData[mt].resistanceHell;
		if ((res & (RESIST_MAGIC | RESIST_FIRE | RESIST_LIGHTNING | IMMUNE_MAGIC | IMMUNE_FIRE | IMMUNE_LIGHTNING)) == 0) {
			AddPanelString(_("No magic resistance"));
		} else {
			if ((res & (RESIST_MAGIC | RESIST_FIRE | RESIST_LIGHTNING)) != 0) {
				std::string resists = std::string(_("Resists:"));
				if ((res & RESIST_MAGIC) != 0)
					AppendStrView(resists, _(" Magic"));
				if ((res & RESIST_FIRE) != 0)
					AppendStrView(resists, _(" Fire"));
				if ((res & RESIST_LIGHTNING) != 0)
					AppendStrView(resists, _(" Lightning"));
				AddPanelString(resists);
			}
			if ((res & (IMMUNE_MAGIC | IMMUNE_FIRE | IMMUNE_LIGHTNING)) != 0) {
				std::string immune = std::string(_("Immune:"));
				if ((res & IMMUNE_MAGIC) != 0)
					AppendStrView(immune, _(" Magic"));
				if ((res & IMMUNE_FIRE) != 0)
					AppendStrView(immune, _(" Fire"));
				if ((res & IMMUNE_LIGHTNING) != 0)
					AppendStrView(immune, _(" Lightning"));
				AddPanelString(immune);
			}
		}
	}
}

void PrintUniqueHistory()
{
	auto &monster = Monsters[pcursmonst];
	if (*sgOptions.Gameplay.showMonsterType) {
		AddPanelString(fmt::format(fmt::runtime(_("Type: {:s}")), GetMonsterTypeText(monster.data())));
	}

	int res = monster.resistance & (RESIST_MAGIC | RESIST_FIRE | RESIST_LIGHTNING | IMMUNE_MAGIC | IMMUNE_FIRE | IMMUNE_LIGHTNING);
	if (res == 0) {
		AddPanelString(_("No resistances"));
		AddPanelString(_("No Immunities"));
	} else {
		if ((res & (RESIST_MAGIC | RESIST_FIRE | RESIST_LIGHTNING)) != 0)
			AddPanelString(_("Some Magic Resistances"));
		else
			AddPanelString(_("No resistances"));
		if ((res & (IMMUNE_MAGIC | IMMUNE_FIRE | IMMUNE_LIGHTNING)) != 0) {
			AddPanelString(_("Some Magic Immunities"));
		} else {
			AddPanelString(_("No Immunities"));
		}
	}
}

void PlayEffect(Monster &monster, MonsterSound mode)
{
	if (MyPlayer->pLvlLoad != 0) {
		return;
	}

	int sndIdx = GenerateRnd(2);
	if (!gbSndInited || !gbSoundOn || gbBufferMsgs != 0) {
		return;
	}

	TSnd *snd = monster.type().sounds[static_cast<size_t>(mode)][sndIdx].get();
	if (snd == nullptr || snd->isPlaying()) {
		return;
	}

	int lVolume = 0;
	int lPan = 0;
	if (!CalculateSoundPosition(monster.position.tile, &lVolume, &lPan))
		return;

	snd_play_snd(snd, lVolume, lPan);
}

void MissToMonst(Missile &missile, Point position)
{
	int monsterId = missile._misource;

	assert(static_cast<size_t>(monsterId) < MaxMonsters);
	auto &monster = Monsters[monsterId];

	Point oldPosition = missile.position.tile;
	dMonster[position.x][position.y] = monsterId + 1;
	monster.direction = static_cast<Direction>(missile._mimfnum);
	monster.position.tile = position;
	M_StartStand(monster, monster.direction);
	M_StartHit(monster, 0);

	if (monster.type().type == MT_GLOOM)
		return;

	if ((monster.flags & MFLAG_TARGETS_MONSTER) == 0) {
		if (dPlayer[oldPosition.x][oldPosition.y] <= 0)
			return;

		int pnum = dPlayer[oldPosition.x][oldPosition.y] - 1;
		Player &player = Players[pnum];
		MonsterAttackPlayer(monster, player, 500, monster.minDamageSpecial, monster.maxDamageSpecial);

		if (IsAnyOf(monster.type().type, MT_NSNAKE, MT_RSNAKE, MT_BSNAKE, MT_GSNAKE))
			return;

		if (player._pmode != PM_GOTHIT && player._pmode != PM_DEATH)
			StartPlrHit(player, 0, true);
		Point newPosition = oldPosition + monster.direction;
		if (PosOkPlayer(player, newPosition)) {
			player.position.tile = newPosition;
			FixPlayerLocation(player, player._pdir);
			FixPlrWalkTags(player);
			dPlayer[newPosition.x][newPosition.y] = pnum + 1;
			SetPlayerOld(player);
		}
		return;
	}

	Monster *target = FindMonsterAtPosition(oldPosition, true);

	if (target == nullptr)
		return;

	MonsterAttackMonster(monster, *target, 500, monster.minDamageSpecial, monster.maxDamageSpecial);

	if (IsAnyOf(monster.type().type, MT_NSNAKE, MT_RSNAKE, MT_BSNAKE, MT_GSNAKE))
		return;

	Point newPosition = oldPosition + monster.direction;
	if (IsTileAvailable(*target, newPosition)) {
		monsterId = dMonster[oldPosition.x][oldPosition.y];
		dMonster[newPosition.x][newPosition.y] = monsterId;
		dMonster[oldPosition.x][oldPosition.y] = 0;
		monsterId--;
		monster.position.tile = newPosition;
		monster.position.future = newPosition;
	}
}

Monster *FindMonsterAtPosition(Point position, bool ignoreMovingMonsters)
{
	if (!InDungeonBounds(position)) {
		return nullptr;
	}

	auto monsterId = dMonster[position.x][position.y];

	if (monsterId == 0 || (ignoreMovingMonsters && monsterId < 0)) {
		// nothing at this position, return a nullptr
		return nullptr;
	}

	return &Monsters[abs(monsterId) - 1];
}

bool IsTileAvailable(const Monster &monster, Point position)
{
	if (!IsTileAvailable(position))
		return false;

	return IsTileSafe(monster, position);
}

bool IsSkel(_monster_id mt)
{
	return std::find(std::begin(SkeletonTypes), std::end(SkeletonTypes), mt) != std::end(SkeletonTypes);
}

bool IsGoat(_monster_id mt)
{
	return IsAnyOf(mt,
	    MT_NGOATMC, MT_BGOATMC, MT_RGOATMC, MT_GGOATMC,
	    MT_NGOATBW, MT_BGOATBW, MT_RGOATBW, MT_GGOATBW);
}

void ActivateSkeleton(Monster &monster, Point position)
{
	if (IsTileAvailable(position)) {
		ActivateSpawn(monster, position, Direction::SouthWest);
		return;
	}

	constexpr std::array<Direction, 8> spawnDirections {
		Direction::North, Direction::NorthEast, Direction::East, Direction::NorthWest, Direction::SouthEast, Direction::West, Direction::SouthWest, Direction::South
	};
	std::bitset<8> spawnOk;

	for (size_t i = 0; i < spawnDirections.size(); i++) {
		if (IsTileAvailable(position + spawnDirections[i]))
			spawnOk.set(i);
	}
	if (spawnOk.none())
		return;

	// this is used in the following loop to find the nth set bit.
	int spawnChoice = GenerateRnd(15) % spawnOk.count();

	for (size_t i = 0; i < spawnOk.size(); i++) {
		if (!spawnOk.test(i))
			continue;

		if (spawnChoice > 0) {
			spawnChoice--;
			continue;
		}

		ActivateSpawn(monster, position + spawnDirections[i], Opposite(spawnDirections[i]));
		return;
	}
}

Monster *PreSpawnSkeleton()
{
	Monster *skeleton = AddSkeleton({ 0, 0 }, Direction::South, false);
	if (skeleton != nullptr)
		M_StartStand(*skeleton, Direction::South);

	return skeleton;
}

void TalktoMonster(Monster &monster)
{
	Player &player = Players[monster.enemy];
	monster.mode = MonsterMode::Talk;
	if (monster.ai != AI_SNOTSPIL && monster.ai != AI_LACHDAN) {
		return;
	}

	if (Quests[Q_LTBANNER].IsAvailable() && Quests[Q_LTBANNER]._qvar1 == 2) {
		if (RemoveInventoryItemById(player, IDI_BANNER)) {
			Quests[Q_LTBANNER]._qactive = QUEST_DONE;
			monster.talkMsg = TEXT_BANNER12;
			monster.goal = MonsterGoal::Inquiring;
		}
	}
	if (Quests[Q_VEIL].IsAvailable() && monster.talkMsg >= TEXT_VEIL9) {
		if (RemoveInventoryItemById(player, IDI_GLDNELIX)) {
			monster.talkMsg = TEXT_VEIL11;
			monster.goal = MonsterGoal::Inquiring;
		}
	}
}

void SpawnGolem(Player &player, Monster &golem, Point position, Missile &missile)
{
	dMonster[position.x][position.y] = golem.getId() + 1;
	golem.position.tile = position;
	golem.position.future = position;
	golem.position.old = position;
	golem.pathCount = 0;
	golem.maxHitPoints = 2 * (320 * missile._mispllvl + player._pMaxMana / 3);
	golem.hitPoints = golem.maxHitPoints;
	golem.armorClass = 25;
	golem.toHit = 5 * (missile._mispllvl + 8) + 2 * player._pLevel;
	golem.minDamage = 2 * (missile._mispllvl + 4);
	golem.maxDamage = 2 * (missile._mispllvl + 8);
	golem.flags |= MFLAG_GOLEM;
	StartSpecialStand(golem, Direction::South);
	UpdateEnemy(golem);
	if (&player == MyPlayer) {
		NetSendCmdGolem(
		    golem.position.tile.x,
		    golem.position.tile.y,
		    golem.direction,
		    golem.enemy,
		    golem.hitPoints,
		    GetLevelForMultiplayer(player));
	}
}

bool CanTalkToMonst(const Monster &monster)
{
	return IsAnyOf(monster.goal, MonsterGoal::Inquiring, MonsterGoal::Talking);
}

int encode_enemy(Monster &monster)
{
	if ((monster.flags & MFLAG_TARGETS_MONSTER) != 0)
		return monster.enemy + MAX_PLRS;

	return monster.enemy;
}

void decode_enemy(Monster &monster, int enemyId)
{
	if (enemyId < MAX_PLRS) {
		monster.flags &= ~MFLAG_TARGETS_MONSTER;
		monster.enemy = enemyId;
		monster.enemyPosition = Players[enemyId].position.future;
	} else {
		monster.flags |= MFLAG_TARGETS_MONSTER;
		enemyId -= MAX_PLRS;
		monster.enemy = enemyId;
		monster.enemyPosition = Monsters[enemyId].position.future;
	}
}

[[nodiscard]] size_t Monster::getId() const
{
	return std::distance<const Monster *>(&Monsters[0], this);
}

Monster *Monster::getLeader() const
{
	if (leader == Monster::NoLeader)
		return nullptr;

	return &Monsters[leader];
}

void Monster::setLeader(const Monster *leader)
{
	if (leader == nullptr) {
		// really we should update this->leader to NoLeader to avoid leaving a dangling reference to a dead monster
		// when passed nullptr. So that buffed minions are drawn with a distinct colour in monhealthbar we leave the
		// reference and hope that no code tries to modify the leader through this instance later.
		leaderRelation = LeaderRelation::None;
		return;
	}

	this->leader = leader->getId();
	leaderRelation = LeaderRelation::Leashed;
	ai = leader->ai;
}

[[nodiscard]] unsigned Monster::distanceToEnemy() const
{
	int mx = position.tile.x - enemyPosition.x;
	int my = position.tile.y - enemyPosition.y;
	return std::max(std::abs(mx), std::abs(my));
}

void Monster::checkStandAnimationIsLoaded(Direction mdir)
{
	if (IsAnyOf(mode, MonsterMode::Stand, MonsterMode::Talk)) {
		direction = mdir;
		changeAnimationData(MonsterGraphic::Stand);
	}
}

void Monster::petrify()
{
	mode = MonsterMode::Petrified;
	animInfo.isPetrified = true;
}

bool Monster::isWalking() const
{
	switch (mode) {
	case MonsterMode::MoveNorthwards:
	case MonsterMode::MoveSouthwards:
	case MonsterMode::MoveSideways:
		return true;
	default:
		return false;
	}
}

bool Monster::isImmune(missile_id missileType) const
{
	missile_resistance missileElement = MissilesData[missileType].mResist;

	if (((resistance & IMMUNE_MAGIC) != 0 && missileElement == MISR_MAGIC)
	    || ((resistance & IMMUNE_FIRE) != 0 && missileElement == MISR_FIRE)
	    || ((resistance & IMMUNE_LIGHTNING) != 0 && missileElement == MISR_LIGHTNING)
	    || ((resistance & IMMUNE_ACID) != 0 && missileElement == MISR_ACID))
		return true;
	if (missileType == MIS_HBOLT && type().type != MT_DIABLO && data().monsterClass != MonsterClass::Undead)
		return true;
	return false;
}

bool Monster::isResistant(missile_id missileType) const
{
	missile_resistance missileElement = MissilesData[missileType].mResist;

	if (((resistance & RESIST_MAGIC) != 0 && missileElement == MISR_MAGIC)
	    || ((resistance & RESIST_FIRE) != 0 && missileElement == MISR_FIRE)
	    || ((resistance & RESIST_LIGHTNING) != 0 && missileElement == MISR_LIGHTNING))
		return true;
	if (gbIsHellfire && missileType == MIS_HBOLT && IsAnyOf(type().type, MT_DIABLO, MT_BONEDEMN))
		return true;
	return false;
}

bool Monster::isPlayerMinion() const
{
	// This could be HasAnyOf(GOLEM) && HasNoneOf(BERSERK), I think referencing the type and player index is more robust though
	return type().type == MT_GOLEM && getId() < Players.size();
}

bool Monster::isPossibleToHit() const
{
	return !(hitPoints >> 6 <= 0
	    || talkMsg != TEXT_NONE
	    || (type().type == MT_ILLWEAV && goal == MonsterGoal::Retreat)
	    || mode == MonsterMode::Charge
	    || (IsAnyOf(type().type, MT_COUNSLR, MT_MAGISTR, MT_CABALIST, MT_ADVOCATE) && goal != MonsterGoal::Normal));
}

void Monster::tag(const Player &tagger)
{
	whoHit |= 1 << tagger.getId();
}

bool Monster::tryLiftGargoyle()
{
	if (ai == AI_GARG && (flags & MFLAG_ALLOW_SPECIAL) != 0) {
		flags &= ~MFLAG_ALLOW_SPECIAL;
		mode = MonsterMode::SpecialMeleeAttack;
		return true;
	}
	return false;
}

unsigned int Monster::toHitSpecial(_difficulty difficulty) const
{
	unsigned int baseToHitSpecial = data().toHitSpecial;
	if (isUnique() && UniqueMonstersData[static_cast<size_t>(uniqueType)].customToHit != 0) {
		baseToHitSpecial = UniqueMonstersData[static_cast<size_t>(uniqueType)].customToHit;
	}

	if (difficulty == DIFF_NIGHTMARE) {
		baseToHitSpecial += NightmareToHitBonus;
	} else if (difficulty == DIFF_HELL) {
		baseToHitSpecial += HellToHitBonus;
	}

	return baseToHitSpecial;
}

} // namespace devilution
