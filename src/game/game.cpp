/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019-2022 OpenTibiaBR <opentibiabr@outlook.com>
 * Repository: https://github.com/opentibiabr/canary
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 * Contributors: https://github.com/opentibiabr/canary/graphs/contributors
 * Website: https://docs.opentibiabr.com/
 */

#include "pch.hpp"

#include "lua/creature/actions.hpp"
#include "items/bed.hpp"
#include "creatures/creature.hpp"
#include "database/databasetasks.hpp"
#include "lua/creature/events.hpp"
#include "lua/callbacks/event_callback.hpp"
#include "lua/callbacks/events_callbacks.hpp"
#include "game/game.hpp"
#include "game/zones/zone.hpp"
#include "lua/global/globalevent.hpp"
#include "io/iologindata.hpp"
#include "io/io_wheel.hpp"
#include "io/iomarket.hpp"
#include "items/items.hpp"
#include "lua/scripts/lua_environment.hpp"
#include "creatures/monsters/monster.hpp"
#include "lua/creature/movement.hpp"
#include "game/scheduling/dispatcher.hpp"
#include "game/scheduling/save_manager.hpp"
#include "server/server.hpp"
#include "creatures/combat/spells.hpp"
#include "lua/creature/talkaction.hpp"
#include "items/weapons/weapons.hpp"
#include "creatures/players/imbuements/imbuements.hpp"
#include "creatures/players/wheel/player_wheel.hpp"
#include "creatures/npcs/npc.hpp"
#include "server/network/webhook/webhook.hpp"
#include "protobuf/appearances.pb.h"
#include "server/network/protocol/protocollogin.hpp"
#include "server/network/protocol/protocolstatus.hpp"
#include "map/spectators.hpp"

#include "kv/kv.hpp"

namespace InternalGame {
	void sendBlockEffect(BlockType_t blockType, CombatType_t combatType, const Position &targetPos, std::shared_ptr<Creature> source) {
		if (blockType == BLOCK_DEFENSE) {
			g_game().addMagicEffect(targetPos, CONST_ME_POFF);
		} else if (blockType == BLOCK_ARMOR) {
			g_game().addMagicEffect(targetPos, CONST_ME_BLOCKHIT);
		} else if (blockType == BLOCK_DODGE) {
			g_game().addMagicEffect(targetPos, CONST_ME_DODGE);
		} else if (blockType == BLOCK_IMMUNITY) {
			uint8_t hitEffect = 0;
			switch (combatType) {
				case COMBAT_UNDEFINEDDAMAGE: {
					return;
				}
				case COMBAT_ENERGYDAMAGE:
				case COMBAT_FIREDAMAGE:
				case COMBAT_PHYSICALDAMAGE:
				case COMBAT_ICEDAMAGE:
				case COMBAT_DEATHDAMAGE: {
					hitEffect = CONST_ME_BLOCKHIT;
					break;
				}
				case COMBAT_EARTHDAMAGE: {
					hitEffect = CONST_ME_GREEN_RINGS;
					break;
				}
				case COMBAT_HOLYDAMAGE: {
					hitEffect = CONST_ME_HOLYDAMAGE;
					break;
				}
				default: {
					hitEffect = CONST_ME_POFF;
					break;
				}
			}
			g_game().addMagicEffect(targetPos, hitEffect);
		}

		if (blockType != BLOCK_NONE) {
			g_game().sendSingleSoundEffect(targetPos, SoundEffect_t::NO_DAMAGE, source);
		}
	}

	bool playerCanUseItemOnHouseTile(std::shared_ptr<Player> player, std::shared_ptr<Item> item) {
		if (!player || !item) {
			return false;
		}

		auto itemTile = item->getTile();
		if (!itemTile) {
			return false;
		}

		if (std::shared_ptr<HouseTile> houseTile = std::dynamic_pointer_cast<HouseTile>(itemTile)) {
			const auto &house = houseTile->getHouse();
			if (!house || !house->isInvited(player)) {
				return false;
			}

			auto isGuest = house->getHouseAccessLevel(player) == HOUSE_GUEST;
			auto itemParentContainer = item->getParent() ? item->getParent()->getContainer() : nullptr;
			auto isItemParentContainerBrowseField = itemParentContainer && itemParentContainer->getID() == ITEM_BROWSEFIELD;
			if (isGuest && isItemParentContainerBrowseField) {
				return false;
			}

			auto realItemParent = item->getRealParent();
			auto isItemInGuestInventory = realItemParent && (realItemParent == player || realItemParent->getContainer());
			if (isGuest && !isItemInGuestInventory && !item->isLadder()) {
				return false;
			}
		}

		return true;
	}

	bool playerCanUseItemWithOnHouseTile(std::shared_ptr<Player> player, std::shared_ptr<Item> item, const Position &toPos, int toStackPos, int toItemId) {
		if (!player || !item) {
			return false;
		}

		auto itemTile = item->getTile();
		if (!itemTile) {
			return false;
		}

		if (g_configManager().getBoolean(ONLY_INVITED_CAN_MOVE_HOUSE_ITEMS, __FUNCTION__)) {
			if (std::shared_ptr<HouseTile> houseTile = std::dynamic_pointer_cast<HouseTile>(itemTile)) {
				const auto &house = houseTile->getHouse();
				std::shared_ptr<Thing> targetThing = g_game().internalGetThing(player, toPos, toStackPos, toItemId, STACKPOS_FIND_THING);
				auto targetItem = targetThing ? targetThing->getItem() : nullptr;
				uint16_t targetId = targetItem ? targetItem->getID() : 0;
				auto invitedCheckUseWith = house && item->getRealParent() && item->getRealParent() != player && (!house->isInvited(player) || house->getHouseAccessLevel(player) == HOUSE_GUEST);
				if (targetId != 0 && targetItem && !targetItem->isDummy() && invitedCheckUseWith) {
					player->sendCancelMessage(RETURNVALUE_CANNOTUSETHISOBJECT);
					return false;
				}
			}
		}

		return true;
	}

	template <typename T>
	T getCustomAttributeValue(std::shared_ptr<Item> item, const std::string &attributeName) {
		static_assert(std::is_integral<T>::value, "T must be an integral type");

		auto attribute = item->getCustomAttribute(attributeName);
		if (!attribute) {
			return 0;
		}

		int64_t value = attribute->getInteger();
		if (value < std::numeric_limits<T>::min() || value > std::numeric_limits<T>::max()) {
			g_logger().error("[{}] value is out of range for the specified type", __FUNCTION__);
			return 0;
		}

		return static_cast<T>(value);
	}
} // Namespace InternalGame

Game::Game() {
	offlineTrainingWindow.choices.emplace_back("Sword Fighting and Shielding", SKILL_SWORD);
	offlineTrainingWindow.choices.emplace_back("Axe Fighting and Shielding", SKILL_AXE);
	offlineTrainingWindow.choices.emplace_back("Club Fighting and Shielding", SKILL_CLUB);
	offlineTrainingWindow.choices.emplace_back("Distance Fighting and Shielding", SKILL_DISTANCE);
	offlineTrainingWindow.choices.emplace_back("Magic Level and Shielding", SKILL_MAGLEVEL);
	offlineTrainingWindow.buttons.emplace_back("Okay", 1);
	offlineTrainingWindow.buttons.emplace_back("Cancel", 0);
	offlineTrainingWindow.defaultEscapeButton = 1;
	offlineTrainingWindow.defaultEnterButton = 0;
	offlineTrainingWindow.priority = true;

	// Create instance of IOWheel to Game class
	m_IOWheel = std::make_unique<IOWheel>();
}

Game::~Game() = default;

void Game::resetMonsters() const {
	for (const auto &[monsterId, monster] : getMonsters()) {
		monster->clearTargetList();
		monster->clearFriendList();
	}
}

void Game::resetNpcs() const {
	// Close shop window from all npcs and reset the shopPlayerSet
	for (const auto &[npcId, npc] : getNpcs()) {
		npc->closeAllShopWindows();
		npc->resetPlayerInteractions();
	}
}

void Game::loadBoostedCreature() {
	auto &db = Database::getInstance();
	const auto result = db.storeQuery("SELECT * FROM `boosted_creature`");
	if (!result) {
		g_logger().warn("[Game::loadBoostedCreature] - "
						"Failed to detect boosted creature database. (CODE 01)");
		return;
	}

	const uint16_t date = result->getNumber<uint16_t>("date");
	const time_t now = time(0);
	tm* ltm = localtime(&now);

	if (date == ltm->tm_mday) {
		setBoostedName(result->getString("boostname"));
		return;
	}

	const uint16_t oldRace = result->getNumber<uint16_t>("raceid");
	const auto monsterlist = getBestiaryList();

	struct MonsterRace {
		uint16_t raceId { 0 };
		std::string name;
	};

	MonsterRace selectedMonster;
	if (!monsterlist.empty()) {
		std::vector<MonsterRace> monsters;
		for (const auto &[raceId, _name] : BestiaryList) {
			if (raceId != oldRace) {
				monsters.emplace_back(raceId, _name);
			}
		}

		if (!monsters.empty()) {
			selectedMonster = monsters[normal_random(0, monsters.size() - 1)];
		}
	}

	if (selectedMonster.raceId == 0) {
		g_logger().warn("[Game::loadBoostedCreature] - "
						"It was not possible to generate a new boosted creature->");
		return;
	}

	const auto monsterType = g_monsters().getMonsterType(selectedMonster.name);
	if (!monsterType) {
		g_logger().warn("[Game::loadBoostedCreature] - "
						"It was not possible to generate a new boosted creature-> Monster '"
						+ selectedMonster.name + "' not found.");
		return;
	}

	setBoostedName(selectedMonster.name);

	auto query = std::string("UPDATE `boosted_creature` SET ")
		+ "`date` = '" + std::to_string(ltm->tm_mday) + "',"
		+ "`boostname` = " + db.escapeString(selectedMonster.name) + ","
		+ "`looktype` = " + std::to_string(monsterType->info.outfit.lookType) + ","
		+ "`lookfeet` = " + std::to_string(monsterType->info.outfit.lookFeet) + ","
		+ "`looklegs` = " + std::to_string(monsterType->info.outfit.lookLegs) + ","
		+ "`lookhead` = " + std::to_string(monsterType->info.outfit.lookHead) + ","
		+ "`lookbody` = " + std::to_string(monsterType->info.outfit.lookBody) + ","
		+ "`lookaddons` = " + std::to_string(monsterType->info.outfit.lookAddons) + ","
		+ "`lookmount` = " + std::to_string(monsterType->info.outfit.lookMount) + ","
		+ "`raceid` = '" + std::to_string(selectedMonster.raceId) + "'";

	if (!db.executeQuery(query)) {
		g_logger().warn("[Game::loadBoostedCreature] - "
						"Failed to detect boosted creature database. (CODE 02)");
	}
}

void Game::start(ServiceManager* manager) {
	// Game client protocols
	manager->add<ProtocolGame>(static_cast<uint16_t>(g_configManager().getNumber(GAME_PORT, __FUNCTION__)));
	manager->add<ProtocolLogin>(static_cast<uint16_t>(g_configManager().getNumber(LOGIN_PORT, __FUNCTION__)));
	// OT protocols
	manager->add<ProtocolStatus>(static_cast<uint16_t>(g_configManager().getNumber(STATUS_PORT, __FUNCTION__)));

	serviceManager = manager;

	time_t now = time(0);
	const tm* tms = localtime(&now);
	int minutes = tms->tm_min;
	lightHour = (minutes * LIGHT_DAY_LENGTH) / 60;

	g_dispatcher().scheduleEvent(EVENT_MS + 1000, std::bind_front(&Game::createFiendishMonsters, this), "Game::createFiendishMonsters");
	g_dispatcher().scheduleEvent(EVENT_MS + 1000, std::bind_front(&Game::createInfluencedMonsters, this), "Game::createInfluencedMonsters");

	g_dispatcher().cycleEvent(EVENT_MS, std::bind_front(&Game::updateForgeableMonsters, this), "Game::updateForgeableMonsters");
	g_dispatcher().cycleEvent(EVENT_LIGHTINTERVAL_MS, std::bind(&Game::checkLight, this), "Game::checkLight");
	g_dispatcher().cycleEvent(EVENT_CHECK_CREATURE_INTERVAL, std::bind(&Game::checkCreatures, this), "Game::checkCreatures");
	g_dispatcher().cycleEvent(EVENT_IMBUEMENT_INTERVAL, std::bind(&Game::checkImbuements, this), "Game::checkImbuements");
	g_dispatcher().cycleEvent(
		EVENT_LUA_GARBAGE_COLLECTION, [this] { g_luaEnvironment().collectGarbage(); }, "Calling GC"
	);
	g_dispatcher().cycleEvent(EVENT_REFRESH_MARKET_PRICES, std::bind_front(&Game::loadItemsPrice, this), "Game::loadItemsPrice");
}

GameState_t Game::getGameState() const {
	return gameState;
}

void Game::setWorldType(WorldType_t type) {
	worldType = type;
}

void Game::setGameState(GameState_t newState) {
	if (gameState == GAME_STATE_SHUTDOWN) {
		return; // this cannot be stopped
	}

	if (gameState == newState) {
		return;
	}

	gameState = newState;
	switch (newState) {
		case GAME_STATE_INIT: {
			loadItemsPrice();

			groups.load();
			g_chat().load();

			// Load monsters and npcs stored by the "loadFromXML" function
			map.spawnsMonster.startup();
			map.spawnsNpc.startup();

			// Load monsters and npcs custom stored by the "loadFromXML" function
			for (int i = 0; i < 50; i++) {
				map.spawnsNpcCustomMaps[i].startup();
				map.spawnsMonsterCustomMaps[i].startup();
			}

			raids.loadFromXml();
			raids.startup();

			mounts.loadFromXml();

			loadMotdNum();
			loadPlayersRecord();

			g_globalEvents().startup();

			// Initialize wheel data
			m_IOWheel->initializeGlobalData();
			break;
		}

		case GAME_STATE_SHUTDOWN: {
			g_globalEvents().execute(GLOBALEVENT_SHUTDOWN);

			// kick all players that are still online
			auto it = players.begin();
			while (it != players.end()) {
				it->second->removePlayer(true);
				it = players.begin();
			}

			saveMotdNum();
			g_saveManager().saveAll();

			g_dispatcher().addEvent(std::bind(&Game::shutdown, this), "Game::shutdown");

			break;
		}

		case GAME_STATE_CLOSED: {
			/* kick all players without the CanAlwaysLogin flag */
			auto it = players.begin();
			while (it != players.end()) {
				if (!it->second->hasFlag(PlayerFlags_t::CanAlwaysLogin)) {
					it->second->removePlayer(true);
					it = players.begin();
				} else {
					++it;
				}
			}

			g_saveManager().saveAll();
			break;
		}

		default:
			break;
	}
}

bool Game::loadItemsPrice() {
	IOMarket::getInstance().updateStatistics();
	std::ostringstream query, marketQuery;
	query << "SELECT DISTINCT `itemtype` FROM `market_offers`;";

	Database &db = Database::getInstance();
	DBResult_ptr result = db.storeQuery(query.str());
	if (!result) {
		return false;
	}

	auto stats = IOMarket::getInstance().getPurchaseStatistics();
	for (const auto &[itemId, itemStats] : stats) {
		std::map<uint8_t, uint64_t> tierToPrice;
		for (const auto &[tier, tierStats] : itemStats) {
			auto averagePrice = tierStats.totalPrice / tierStats.numTransactions;
			tierToPrice[tier] = averagePrice;
		}
		itemsPriceMap[itemId] = tierToPrice;
	}
	auto offers = IOMarket::getInstance().getActiveOffers(MARKETACTION_BUY);
	for (const auto &offer : offers) {
		itemsPriceMap[offer.itemId][offer.tier] = std::max(itemsPriceMap[offer.itemId][offer.tier], offer.price);
	}

	return true;
}

void Game::loadMainMap(const std::string &filename) {
	Monster::despawnRange = g_configManager().getNumber(DEFAULT_DESPAWNRANGE, __FUNCTION__);
	Monster::despawnRadius = g_configManager().getNumber(DEFAULT_DESPAWNRADIUS, __FUNCTION__);
	map.loadMap(g_configManager().getString(DATA_DIRECTORY, __FUNCTION__) + "/world/" + filename + ".otbm", true, true, true, true, true);
}

void Game::loadCustomMaps(const std::filesystem::path &customMapPath) {
	Monster::despawnRange = g_configManager().getNumber(DEFAULT_DESPAWNRANGE, __FUNCTION__);
	Monster::despawnRadius = g_configManager().getNumber(DEFAULT_DESPAWNRADIUS, __FUNCTION__);

	namespace fs = std::filesystem;

	if (!fs::exists(customMapPath) && !fs::create_directory(customMapPath)) {
		throw std::ios_base::failure(fmt::format("Failed to create custom map directory {}", customMapPath.string()));
	}

	int customMapIndex = 0;
	for (const auto &entry : fs::directory_iterator(customMapPath)) {
		const auto realPath = entry.path();

		if (realPath.extension() != ".otbm") {
			continue;
		}

		std::string filename = realPath.stem().string();

		// Do not load more maps than possible
		if (customMapIndex >= 50) {
			g_logger().warn("Maximum number of custom maps loaded. Custom map {} [ignored]", filename);
			continue;
		}

		// Filenames that start with a # are ignored.
		if (filename.at(0) == '#') {
			g_logger().info("Custom map {} [disabled]", filename);
			continue;
		}

		// Avoid loading main map again.
		if (filename == g_configManager().getString(MAP_NAME, __FUNCTION__)) {
			g_logger().warn("Custom map {} is main map", filename);
			continue;
		}

		map.loadMapCustom(filename, true, true, true, true, customMapIndex);

		customMapIndex++;
	}

	// Must be done after all maps have been loaded
	map.loadHouseInfo();
}

void Game::loadMap(const std::string &path, const Position &pos) {
	map.loadMap(path, false, false, false, false, false, pos);
}

std::shared_ptr<Cylinder> Game::internalGetCylinder(std::shared_ptr<Player> player, const Position &pos) {
	if (pos.x != 0xFFFF) {
		return map.getTile(pos);
	}

	// container
	if (pos.y & 0x40) {
		uint8_t from_cid = pos.y & 0x0F;
		return player->getContainerByID(from_cid);
	}

	// inventory
	return player;
}

std::shared_ptr<Thing> Game::internalGetThing(std::shared_ptr<Player> player, const Position &pos, int32_t index, uint32_t itemId, StackPosType_t type) {
	if (pos.x != 0xFFFF) {
		std::shared_ptr<Tile> tile = map.getTile(pos);
		if (!tile) {
			return nullptr;
		}

		std::shared_ptr<Thing> thing;
		switch (type) {
			case STACKPOS_LOOK: {
				return tile->getTopVisibleThing(player);
			}

			case STACKPOS_MOVE: {
				std::shared_ptr<Item> item = tile->getTopDownItem();
				if (item && item->isMoveable()) {
					thing = item;
				} else {
					thing = tile->getTopVisibleCreature(player);
				}
				break;
			}

			case STACKPOS_USEITEM: {
				thing = tile->getUseItem(index);
				break;
			}

			case STACKPOS_TOPDOWN_ITEM: {
				thing = tile->getTopDownItem();
				break;
			}

			case STACKPOS_USETARGET: {
				thing = tile->getTopVisibleCreature(player);
				if (!thing) {
					thing = tile->getUseItem(index);
				}
				break;
			}

			case STACKPOS_FIND_THING: {
				thing = tile->getUseItem(index);
				if (!thing) {
					thing = tile->getDoorItem();
				}

				if (!thing) {
					thing = tile->getTopDownItem();
				}

				break;
			}

			default: {
				thing = nullptr;
				break;
			}
		}

		if (player && tile->hasFlag(TILESTATE_SUPPORTS_HANGABLE)) {
			// do extra checks here if the thing is accessable
			if (thing && thing->getItem()) {
				if (tile->hasProperty(CONST_PROP_ISVERTICAL)) {
					if (player->getPosition().x + 1 == tile->getPosition().x && thing->getItem()->isHangable()) {
						thing = nullptr;
					}
				} else { // horizontal
					if (player->getPosition().y + 1 == tile->getPosition().y && thing->getItem()->isHangable()) {
						thing = nullptr;
					}
				}
			}
		}
		return thing;
	}

	// container
	if (pos.y & 0x40) {
		uint8_t fromCid = pos.y & 0x0F;

		std::shared_ptr<Container> parentContainer = player->getContainerByID(fromCid);
		if (!parentContainer) {
			return nullptr;
		}

		if (parentContainer->getID() == ITEM_BROWSEFIELD) {
			std::shared_ptr<Tile> tile = parentContainer->getTile();
			if (tile && tile->hasFlag(TILESTATE_SUPPORTS_HANGABLE)) {
				if (tile->hasProperty(CONST_PROP_ISVERTICAL)) {
					if (player->getPosition().x + 1 == tile->getPosition().x) {
						return nullptr;
					}
				} else { // horizontal
					if (player->getPosition().y + 1 == tile->getPosition().y) {
						return nullptr;
					}
				}
			}
		}

		uint8_t slot = pos.z;
		auto containerIndex = player->getContainerIndex(fromCid) + slot;
		if (parentContainer->isStoreInboxFiltered()) {
			return parentContainer->getFilteredItemByIndex(containerIndex);
		}

		return parentContainer->getItemByIndex(containerIndex);
	} else if (pos.y == 0x20 || pos.y == 0x21) {
		// '0x20' -> From depot.
		// '0x21' -> From inbox.
		// Both only when the item is from depot search window.
		if (!player->isDepotSearchOpenOnItem(static_cast<uint16_t>(itemId))) {
			player->sendCancelMessage(RETURNVALUE_NOTPOSSIBLE);
			return nullptr;
		}

		return player->getItemFromDepotSearch(static_cast<uint16_t>(itemId), pos);
	} else if (pos.y == 0 && pos.z == 0) {
		const ItemType &it = Item::items[itemId];
		if (it.id == 0) {
			return nullptr;
		}

		int32_t subType;
		if (it.isFluidContainer()) {
			subType = index;
		} else {
			subType = -1;
		}

		return findItemOfType(player, it.id, true, subType);
	}

	// inventory
	Slots_t slot = static_cast<Slots_t>(pos.y);
	return player->getInventoryItem(slot);
}

void Game::internalGetPosition(std::shared_ptr<Item> item, Position &pos, uint8_t &stackpos) {
	pos.x = 0;
	pos.y = 0;
	pos.z = 0;
	stackpos = 0;

	std::shared_ptr<Cylinder> topParent = item->getTopParent();
	if (topParent) {
		if (std::shared_ptr<Player> player = std::dynamic_pointer_cast<Player>(topParent)) {
			pos.x = 0xFFFF;

			std::shared_ptr<Container> container = std::dynamic_pointer_cast<Container>(item->getParent());
			if (container) {
				pos.y = static_cast<uint16_t>(0x40) | static_cast<uint16_t>(player->getContainerID(container));
				pos.z = container->getThingIndex(item);
				stackpos = pos.z;
			} else {
				pos.y = player->getThingIndex(item);
				stackpos = pos.y;
			}
		} else if (std::shared_ptr<Tile> tile = topParent->getTile()) {
			pos = tile->getPosition();
			stackpos = tile->getThingIndex(item);
		}
	}
}

std::shared_ptr<Creature> Game::getCreatureByID(uint32_t id) {
	if (id >= Player::getFirstID() && id <= Player::getLastID()) {
		return getPlayerByID(id);
	} else if (id <= Monster::monsterAutoID) {
		return getMonsterByID(id);
	} else if (id <= Npc::npcAutoID) {
		return getNpcByID(id);
	} else {
		g_logger().warn("Creature with id {} not exists");
	}
	return nullptr;
}

std::shared_ptr<Monster> Game::getMonsterByID(uint32_t id) {
	if (id == 0) {
		return nullptr;
	}

	auto it = monsters.find(id);
	if (it == monsters.end()) {
		return nullptr;
	}
	return it->second;
}

std::shared_ptr<Npc> Game::getNpcByID(uint32_t id) {
	if (id == 0) {
		return nullptr;
	}

	auto it = npcs.find(id);
	if (it == npcs.end()) {
		return nullptr;
	}
	return it->second;
}

std::shared_ptr<Player> Game::getPlayerByID(uint32_t id, bool allowOffline /* = false */) {
	auto playerMap = players.find(id);
	if (playerMap != players.end()) {
		return playerMap->second;
	}

	if (!allowOffline) {
		return nullptr;
	}
	std::shared_ptr<Player> tmpPlayer = std::make_shared<Player>(nullptr);
	if (!IOLoginData::loadPlayerById(tmpPlayer, id)) {
		return nullptr;
	}
	tmpPlayer->setOnline(false);
	return tmpPlayer;
}

std::shared_ptr<Creature> Game::getCreatureByName(const std::string &s) {
	if (s.empty()) {
		return nullptr;
	}

	const std::string &lowerCaseName = asLowerCaseString(s);

	auto m_it = mappedPlayerNames.find(lowerCaseName);
	if (m_it != mappedPlayerNames.end()) {
		return m_it->second.lock();
	}

	for (const auto &it : npcs) {
		if (lowerCaseName == asLowerCaseString(it.second->getName())) {
			return it.second;
		}
	}

	for (const auto &it : monsters) {
		if (lowerCaseName == asLowerCaseString(it.second->getName())) {
			return it.second;
		}
	}
	return nullptr;
}

std::shared_ptr<Npc> Game::getNpcByName(const std::string &s) {
	if (s.empty()) {
		return nullptr;
	}

	const char* npcName = s.c_str();
	for (const auto &it : npcs) {
		if (strcasecmp(npcName, it.second->getName().c_str()) == 0) {
			return it.second;
		}
	}
	return nullptr;
}

std::shared_ptr<Player> Game::getPlayerByName(const std::string &s, bool allowOffline /* = false */) {
	if (s.empty()) {
		return nullptr;
	}

	auto it = mappedPlayerNames.find(asLowerCaseString(s));
	if (it == mappedPlayerNames.end() || it->second.expired()) {
		if (!allowOffline) {
			return nullptr;
		}
		std::shared_ptr<Player> tmpPlayer = std::make_shared<Player>(nullptr);
		if (!IOLoginData::loadPlayerByName(tmpPlayer, s)) {
			g_logger().error("Failed to load player {} from database", s);
			return nullptr;
		}
		tmpPlayer->setOnline(false);
		return tmpPlayer;
	}
	return it->second.lock();
}

std::shared_ptr<Player> Game::getPlayerByGUID(const uint32_t &guid, bool allowOffline /* = false */) {
	if (guid == 0) {
		return nullptr;
	}
	for (const auto &it : players) {
		if (guid == it.second->getGUID()) {
			return it.second;
		}
	}
	if (!allowOffline) {
		return nullptr;
	}
	std::shared_ptr<Player> tmpPlayer = std::make_shared<Player>(nullptr);
	if (!IOLoginData::loadPlayerById(tmpPlayer, guid)) {
		return nullptr;
	}
	tmpPlayer->setOnline(false);
	return tmpPlayer;
}

std::string Game::getPlayerNameByGUID(const uint32_t &guid) {
	if (guid == 0) {
		return "";
	}
	if (m_playerNameCache.contains(guid)) {
		return m_playerNameCache.at(guid);
	}
	auto player = getPlayerByGUID(guid, true);
	auto name = player ? player->getName() : "";
	if (!name.empty()) {
		m_playerNameCache[guid] = name;
	}
	return name;
}

ReturnValue Game::getPlayerByNameWildcard(const std::string &s, std::shared_ptr<Player> &player) {
	size_t strlen = s.length();
	if (strlen == 0 || strlen > 20) {
		return RETURNVALUE_PLAYERWITHTHISNAMEISNOTONLINE;
	}

	if (s.back() == '~') {
		const std::string &query = asLowerCaseString(s.substr(0, strlen - 1));
		std::string result;
		ReturnValue ret = wildcardTree.findOne(query, result);
		if (ret != RETURNVALUE_NOERROR) {
			return ret;
		}

		player = getPlayerByName(result);
	} else {
		player = getPlayerByName(s);
	}

	if (!player) {
		return RETURNVALUE_PLAYERWITHTHISNAMEISNOTONLINE;
	}

	return RETURNVALUE_NOERROR;
}

std::shared_ptr<Player> Game::getPlayerByAccount(uint32_t acc) {
	for (const auto &it : players) {
		if (it.second->getAccountId() == acc) {
			return it.second;
		}
	}
	return nullptr;
}

bool Game::internalPlaceCreature(std::shared_ptr<Creature> creature, const Position &pos, bool extendedPos /*=false*/, bool forced /*= false*/, bool creatureCheck /*= false*/) {
	if (creature->getParent() != nullptr) {
		return false;
	}
	const auto &tile = map.getTile(pos);
	if (!tile) {
		return false;
	}
	auto toZones = tile->getZones();
	if (auto ret = beforeCreatureZoneChange(creature, {}, toZones); ret != RETURNVALUE_NOERROR) {
		return false;
	}

	if (!map.placeCreature(pos, creature, extendedPos, forced)) {
		return false;
	}

	creature->setID();
	creature->addList();
	creature->updateCalculatedStepSpeed();

	if (creatureCheck) {
		addCreatureCheck(creature);
		creature->onPlacedCreature();
	}
	afterCreatureZoneChange(creature, {}, toZones);
	return true;
}

bool Game::placeCreature(std::shared_ptr<Creature> creature, const Position &pos, bool extendedPos /*=false*/, bool forced /*= false*/) {
	metrics::method_latency measure(__METHOD_NAME__);
	if (!internalPlaceCreature(creature, pos, extendedPos, forced)) {
		return false;
	}

	bool hasPlayerSpectators = false;
	for (const auto &spectator : Spectators().find<Creature>(creature->getPosition(), true)) {
		if (const auto &tmpPlayer = spectator->getPlayer()) {
			tmpPlayer->sendCreatureAppear(creature, creature->getPosition(), true);
			hasPlayerSpectators = true;
		}
		spectator->onCreatureAppear(creature, true);
	}

	if (hasPlayerSpectators) {
		addCreatureCheck(creature);
	}

	auto parent = creature->getParent();
	if (parent) {
		parent->postAddNotification(creature, nullptr, 0);
	}
	creature->onPlacedCreature();
	return true;
}

bool Game::removeCreature(std::shared_ptr<Creature> creature, bool isLogout /* = true*/) {
	metrics::method_latency measure(__METHOD_NAME__);
	if (!creature || creature->isRemoved()) {
		return false;
	}

	std::shared_ptr<Tile> tile = creature->getTile();
	if (!tile) {
		g_logger().error("[{}] tile on position '{}' for creature '{}' not exist", __FUNCTION__, creature->getPosition().toString(), creature->getName());
	}
	auto fromZones = creature->getZones();

	if (tile) {
		std::vector<int32_t> oldStackPosVector;
		auto spectators = Spectators().find<Creature>(tile->getPosition(), true);
		auto playersSpectators = spectators.filter<Player>();

		for (const auto &spectator : playersSpectators) {
			if (const auto &player = spectator->getPlayer()) {
				oldStackPosVector.push_back(player->canSeeCreature(creature) ? tile->getStackposOfCreature(player, creature) : -1);
			}
		}

		tile->removeCreature(creature);

		const Position &tilePosition = tile->getPosition();

		// Send to client
		size_t i = 0;
		for (const auto &spectator : playersSpectators) {
			if (const auto &player = spectator->getPlayer()) {
				player->sendRemoveTileThing(tilePosition, oldStackPosVector[i++]);
			}
		}

		// event method
		for (auto spectator : spectators) {
			spectator->onRemoveCreature(creature, isLogout);
		}
	}

	if (creature->getMaster() && !creature->getMaster()->isRemoved()) {
		creature->setMaster(nullptr);
	}

	creature->getParent()->postRemoveNotification(creature, nullptr, 0);
	afterCreatureZoneChange(creature, fromZones, {});

	creature->removeList();
	creature->setRemoved();

	removeCreatureCheck(creature);

	for (auto summon : creature->getSummons()) {
		summon->setSkillLoss(false);
		removeCreature(summon);
	}

	if (creature->getPlayer() && isLogout) {
		auto it = teamFinderMap.find(creature->getPlayer()->getGUID());
		if (it != teamFinderMap.end()) {
			teamFinderMap.erase(it);
		}
	}

	return true;
}

void Game::executeDeath(uint32_t creatureId) {
	metrics::method_latency measure(__METHOD_NAME__);
	std::shared_ptr<Creature> creature = getCreatureByID(creatureId);
	if (creature && !creature->isRemoved()) {
		afterCreatureZoneChange(creature, creature->getZones(), {});
		creature->onDeath();
	}
}

void Game::playerTeleport(uint32_t playerId, const Position &newPosition) {
	metrics::method_latency measure(__METHOD_NAME__);
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player || !player->hasFlag(PlayerFlags_t::CanMapClickTeleport)) {
		return;
	}

	ReturnValue returnValue = g_game().internalTeleport(player, newPosition, false);
	if (returnValue != RETURNVALUE_NOERROR) {
		player->sendCancelMessage(returnValue);
	}
}

void Game::playerInspectItem(std::shared_ptr<Player> player, const Position &pos) {
	metrics::method_latency measure(__METHOD_NAME__);
	std::shared_ptr<Thing> thing = internalGetThing(player, pos, 0, 0, STACKPOS_TOPDOWN_ITEM);
	if (!thing) {
		player->sendCancelMessage(RETURNVALUE_NOTPOSSIBLE);
		return;
	}

	std::shared_ptr<Item> item = thing->getItem();
	if (!item) {
		player->sendCancelMessage(RETURNVALUE_NOTPOSSIBLE);
		return;
	}

	player->sendItemInspection(item->getID(), static_cast<uint8_t>(item->getItemCount()), item, false);
}

void Game::playerInspectItem(std::shared_ptr<Player> player, uint16_t itemId, uint8_t itemCount, bool cyclopedia) {
	metrics::method_latency measure(__METHOD_NAME__);
	player->sendItemInspection(itemId, itemCount, nullptr, cyclopedia);
}

FILELOADER_ERRORS Game::loadAppearanceProtobuf(const std::string &file) {
	using namespace Canary::protobuf::appearances;

	std::fstream fileStream(file, std::ios::in | std::ios::binary);
	if (!fileStream.is_open()) {
		g_logger().error("[Game::loadAppearanceProtobuf] - Failed to load {}, file cannot be oppened", file);
		fileStream.close();
		return ERROR_NOT_OPEN;
	}

	// Verify that the version of the library that we linked against is
	// compatible with the version of the headers we compiled against.
	GOOGLE_PROTOBUF_VERIFY_VERSION;
	appearances = Appearances();
	if (!appearances.ParseFromIstream(&fileStream)) {
		g_logger().error("[Game::loadAppearanceProtobuf] - Failed to parse binary file {}, file is invalid", file);
		fileStream.close();
		return ERROR_NOT_OPEN;
	}

	// Parsing all items into ItemType
	Item::items.loadFromProtobuf();

	// Only iterate other objects if necessary
	if (g_configManager().getBoolean(WARN_UNSAFE_SCRIPTS, __FUNCTION__)) {
		// Registering distance effects
		for (uint32_t it = 0; it < appearances.effect_size(); it++) {
			registeredMagicEffects.push_back(static_cast<uint16_t>(appearances.effect(it).id()));
		}

		// Registering missile effects
		for (uint32_t it = 0; it < appearances.missile_size(); it++) {
			registeredDistanceEffects.push_back(static_cast<uint16_t>(appearances.missile(it).id()));
		}

		// Registering outfits
		for (uint32_t it = 0; it < appearances.outfit_size(); it++) {
			registeredLookTypes.push_back(static_cast<uint16_t>(appearances.outfit(it).id()));
		}
	}

	fileStream.close();

	// Disposing allocated objects.
	google::protobuf::ShutdownProtobufLibrary();

	return ERROR_NONE;
}

void Game::playerMoveThing(uint32_t playerId, const Position &fromPos, uint16_t itemId, uint8_t fromStackPos, const Position &toPos, uint8_t count) {
	metrics::method_latency measure(__METHOD_NAME__);
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	// Prevent the player from being able to move the item within the imbuement window
	if (player->hasImbuingItem()) {
		return;
	}

	uint8_t fromIndex = 0;
	if (fromPos.x == 0xFFFF) {
		if (fromPos.y & 0x40) {
			fromIndex = fromPos.z;
		} else if ((fromPos.y == 0x20 || fromPos.y == 0x21) && !player->isDepotSearchOpenOnItem(itemId)) {
			// '0x20' -> From depot.
			// '0x21' -> From inbox.
			// Both only when the item is being moved from depot search window.
			player->sendCancelMessage(RETURNVALUE_NOTPOSSIBLE);
			return;
		} else {
			fromIndex = static_cast<uint8_t>(fromPos.y);
		}
	} else {
		fromIndex = fromStackPos;
	}

	std::shared_ptr<Thing> thing = internalGetThing(player, fromPos, fromIndex, itemId, STACKPOS_MOVE);
	if (!thing) {
		player->sendCancelMessage(RETURNVALUE_NOTPOSSIBLE);
		return;
	}

	if (std::shared_ptr<Creature> movingCreature = thing->getCreature()) {
		std::shared_ptr<Tile> tile = map.getTile(toPos);
		if (!tile) {
			player->sendCancelMessage(RETURNVALUE_NOTPOSSIBLE);
			return;
		}

		if (Position::areInRange<1, 1, 0>(movingCreature->getPosition(), player->getPosition())) {
			std::shared_ptr<Task> task = createPlayerTask(
				g_configManager().getNumber(PUSH_DELAY, __FUNCTION__),
				std::bind(&Game::playerMoveCreatureByID, this, player->getID(), movingCreature->getID(), movingCreature->getPosition(), tile->getPosition()),
				"Game::playerMoveCreatureByID"
			);
			player->setNextActionPushTask(task);
		} else {
			playerMoveCreature(player, movingCreature, movingCreature->getPosition(), tile);
		}
	} else if (thing->getItem()) {
		std::shared_ptr<Cylinder> toCylinder = internalGetCylinder(player, toPos);
		if (!toCylinder) {
			player->sendCancelMessage(RETURNVALUE_NOTPOSSIBLE);
			return;
		}

		playerMoveItem(player, fromPos, itemId, fromStackPos, toPos, count, thing->getItem(), toCylinder);
	}
}

void Game::playerMoveCreatureByID(uint32_t playerId, uint32_t movingCreatureId, const Position &movingCreatureOrigPos, const Position &toPos) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	std::shared_ptr<Creature> movingCreature = getCreatureByID(movingCreatureId);
	if (!movingCreature) {
		return;
	}

	std::shared_ptr<Tile> toTile = map.getTile(toPos);
	if (!toTile) {
		player->sendCancelMessage(RETURNVALUE_NOTPOSSIBLE);
		return;
	}

	playerMoveCreature(player, movingCreature, movingCreatureOrigPos, toTile);
}

void Game::playerMoveCreature(std::shared_ptr<Player> player, std::shared_ptr<Creature> movingCreature, const Position &movingCreatureOrigPos, std::shared_ptr<Tile> toTile) {
	metrics::method_latency measure(__METHOD_NAME__);
	if (!player->canDoAction()) {
		uint32_t delay = 600;
		std::shared_ptr<Task> task = createPlayerTask(delay, std::bind(&Game::playerMoveCreatureByID, this, player->getID(), movingCreature->getID(), movingCreatureOrigPos, toTile->getPosition()), "Game::playerMoveCreatureByID");

		player->setNextActionPushTask(task);
		return;
	}

	player->setNextActionTask(nullptr);

	if (!Position::areInRange<1, 1, 0>(movingCreatureOrigPos, player->getPosition())) {
		// need to walk to the creature first before moving it
		stdext::arraylist<Direction> listDir(128);
		if (player->getPathTo(movingCreatureOrigPos, listDir, 0, 1, true, true)) {
			g_dispatcher().addEvent(std::bind(&Game::playerAutoWalk, this, player->getID(), listDir.data()), "Game::playerAutoWalk");

			std::shared_ptr<Task> task = createPlayerTask(600, std::bind(&Game::playerMoveCreatureByID, this, player->getID(), movingCreature->getID(), movingCreatureOrigPos, toTile->getPosition()), "Game::playerMoveCreatureByID");

			player->pushEvent(true);
			player->setNextActionPushTask(task);
		} else {
			player->sendCancelMessage(RETURNVALUE_THEREISNOWAY);
		}
		return;
	}

	player->pushEvent(false);
	std::shared_ptr<Monster> monster = movingCreature->getMonster();
	bool isFamiliar = false;
	if (monster) {
		isFamiliar = monster->isFamiliar();
	}

	if (!isFamiliar && ((!movingCreature->isPushable() && !player->hasFlag(PlayerFlags_t::CanPushAllCreatures)) || (movingCreature->isInGhostMode() && !player->isAccessPlayer()))) {
		player->sendCancelMessage(RETURNVALUE_NOTMOVEABLE);
		return;
	}

	// check throw distance
	const Position &movingCreaturePos = movingCreature->getPosition();
	const Position &toPos = toTile->getPosition();
	if ((Position::getDistanceX(movingCreaturePos, toPos) > movingCreature->getThrowRange()) || (Position::getDistanceY(movingCreaturePos, toPos) > movingCreature->getThrowRange()) || (Position::getDistanceZ(movingCreaturePos, toPos) * 4 > movingCreature->getThrowRange())) {
		player->sendCancelMessage(RETURNVALUE_DESTINATIONOUTOFREACH);
		return;
	}

	if (player != movingCreature) {
		if (toTile->hasFlag(TILESTATE_BLOCKPATH)) {
			player->sendCancelMessage(RETURNVALUE_NOTENOUGHROOM);
			return;
		} else if ((movingCreature->getZoneType() == ZONE_PROTECTION && !toTile->hasFlag(TILESTATE_PROTECTIONZONE)) || (movingCreature->getZoneType() == ZONE_NOPVP && !toTile->hasFlag(TILESTATE_NOPVPZONE))) {
			player->sendCancelMessage(RETURNVALUE_NOTPOSSIBLE);
			return;
		} else {
			if (CreatureVector* tileCreatures = toTile->getCreatures()) {
				for (auto &tileCreature : *tileCreatures) {
					if (!tileCreature->isInGhostMode()) {
						player->sendCancelMessage(RETURNVALUE_NOTENOUGHROOM);
						return;
					}
				}
			}

			auto movingNpc = movingCreature->getNpc();
			if (movingNpc && movingNpc->canInteract(toPos)) {
				player->sendCancelMessage(RETURNVALUE_NOTENOUGHROOM);
				return;
			}
		}

		movingCreature->setLastPosition(movingCreature->getPosition());
	}

	if (!g_events().eventPlayerOnMoveCreature(player, movingCreature, movingCreaturePos, toPos)) {
		return;
	}

	if (!g_callbacks().checkCallback(EventCallback_t::playerOnMoveCreature, &EventCallback::playerOnMoveCreature, player, movingCreature, movingCreaturePos, toPos)) {
		return;
	}

	ReturnValue ret = internalMoveCreature(movingCreature, toTile);
	if (ret != RETURNVALUE_NOERROR) {
		player->sendCancelMessage(ret);
	}
	player->setLastPosition(player->getPosition());
}

ReturnValue Game::internalMoveCreature(std::shared_ptr<Creature> creature, Direction direction, uint32_t flags /*= 0*/) {
	if (!creature) {
		return RETURNVALUE_NOTPOSSIBLE;
	}

	creature->setLastPosition(creature->getPosition());
	const Position &currentPos = creature->getPosition();
	Position destPos = getNextPosition(direction, currentPos);
	std::shared_ptr<Player> player = creature->getPlayer();

	bool diagonalMovement = (direction & DIRECTION_DIAGONAL_MASK) != 0;
	if (player && !diagonalMovement) {
		// try go up
		auto tile = creature->getTile();
		if (currentPos.z != 8 && tile && tile->hasHeight(3)) {
			std::shared_ptr<Tile> tmpTile = map.getTile(currentPos.x, currentPos.y, currentPos.getZ() - 1);
			if (tmpTile == nullptr || (tmpTile->getGround() == nullptr && !tmpTile->hasFlag(TILESTATE_BLOCKSOLID))) {
				tmpTile = map.getTile(destPos.x, destPos.y, destPos.getZ() - 1);
				if (tmpTile && tmpTile->getGround() && !tmpTile->hasFlag(TILESTATE_BLOCKSOLID)) {
					flags |= FLAG_IGNOREBLOCKITEM | FLAG_IGNOREBLOCKCREATURE;

					if (!tmpTile->hasFlag(TILESTATE_FLOORCHANGE)) {
						player->setDirection(direction);
						destPos.z--;
					}
				}
			}
		}

		// try go down
		if (currentPos.z != 7 && currentPos.z == destPos.z) {
			std::shared_ptr<Tile> tmpTile = map.getTile(destPos.x, destPos.y, destPos.z);
			if (tmpTile == nullptr || (tmpTile->getGround() == nullptr && !tmpTile->hasFlag(TILESTATE_BLOCKSOLID))) {
				tmpTile = map.getTile(destPos.x, destPos.y, destPos.z + 1);
				if (tmpTile && tmpTile->hasHeight(3)) {
					flags |= FLAG_IGNOREBLOCKITEM | FLAG_IGNOREBLOCKCREATURE;
					player->setDirection(direction);
					destPos.z++;
				}
			}
		}
	}

	std::shared_ptr<Tile> toTile = map.getTile(destPos);
	if (!toTile) {
		return RETURNVALUE_NOTPOSSIBLE;
	}
	return internalMoveCreature(creature, toTile, flags);
}

ReturnValue Game::internalMoveCreature(const std::shared_ptr<Creature> &creature, const std::shared_ptr<Tile> &toTile, uint32_t flags /*= 0*/) {
	metrics::method_latency measure(__METHOD_NAME__);
	if (creature->hasCondition(CONDITION_ROOTED)) {
		return RETURNVALUE_NOTPOSSIBLE;
	}

	// check if we can move the creature to the destination
	ReturnValue ret = toTile->queryAdd(0, creature, 1, flags);
	if (ret != RETURNVALUE_NOERROR) {
		return ret;
	}

	if (creature->hasCondition(CONDITION_ROOTED)) {
		return RETURNVALUE_NOTPOSSIBLE;
	}

	if (creature->hasCondition(CONDITION_FEARED)) {
		std::shared_ptr<MagicField> field = toTile->getFieldItem();
		if (field && !field->isBlocking() && field->getDamage() != 0) {
			return RETURNVALUE_NOTPOSSIBLE;
		}
	}

	map.moveCreature(creature, toTile);
	if (creature->getParent() != toTile) {
		return RETURNVALUE_NOERROR;
	}

	int32_t index = 0;
	std::shared_ptr<Item> toItem = nullptr;
	std::shared_ptr<Tile> subCylinder = nullptr;
	std::shared_ptr<Tile> toCylinder = toTile;
	std::shared_ptr<Tile> fromCylinder = nullptr;
	uint32_t n = 0;

	while ((subCylinder = toCylinder->queryDestination(index, creature, &toItem, flags)->getTile()) != toCylinder) {
		map.moveCreature(creature, subCylinder);

		if (creature->getParent() != subCylinder) {
			// could happen if a script move the creature
			fromCylinder = nullptr;
			break;
		}

		fromCylinder = toCylinder;
		toCylinder = subCylinder;
		flags = 0;

		// to prevent infinite loop
		if (++n >= MAP_MAX_LAYERS) {
			break;
		}
	}

	if (fromCylinder) {
		const Position &fromPosition = fromCylinder->getPosition();
		const Position &toPosition = toCylinder->getPosition();
		if (fromPosition.z != toPosition.z && (fromPosition.x != toPosition.x || fromPosition.y != toPosition.y)) {
			Direction dir = getDirectionTo(fromPosition, toPosition);
			if ((dir & DIRECTION_DIAGONAL_MASK) == 0) {
				internalCreatureTurn(creature, dir);
			}
		}
	}

	return RETURNVALUE_NOERROR;
}

void Game::playerMoveItemByPlayerID(uint32_t playerId, const Position &fromPos, uint16_t itemId, uint8_t fromStackPos, const Position &toPos, uint8_t count) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}
	playerMoveItem(player, fromPos, itemId, fromStackPos, toPos, count, nullptr, nullptr);
}

void Game::playerMoveItem(std::shared_ptr<Player> player, const Position &fromPos, uint16_t itemId, uint8_t fromStackPos, const Position &toPos, uint8_t count, std::shared_ptr<Item> item, std::shared_ptr<Cylinder> toCylinder) {
	if (!player->canDoAction()) {
		uint32_t delay = player->getNextActionTime();
		std::shared_ptr<Task> task = createPlayerTask(delay, std::bind(&Game::playerMoveItemByPlayerID, this, player->getID(), fromPos, itemId, fromStackPos, toPos, count), "Game::playerMoveItemByPlayerID");
		player->setNextActionTask(task);
		return;
	}

	player->setNextActionTask(nullptr);

	if (item == nullptr) {
		uint8_t fromIndex = 0;
		if (fromPos.x == 0xFFFF) {
			if (fromPos.y & 0x40) {
				fromIndex = fromPos.z;
			} else if ((fromPos.y == 0x20 || fromPos.y == 0x21) && !player->isDepotSearchOpenOnItem(itemId)) {
				// '0x20' -> From depot.
				// '0x21' -> From inbox.
				// Both only when the item is being moved from depot search window.
				player->sendCancelMessage(RETURNVALUE_NOTPOSSIBLE);
				return;
			} else {
				fromIndex = static_cast<uint8_t>(fromPos.y);
			}
		} else {
			fromIndex = fromStackPos;
		}

		std::shared_ptr<Thing> thing = internalGetThing(player, fromPos, fromIndex, itemId, STACKPOS_MOVE);
		if (!thing || !thing->getItem()) {
			player->sendCancelMessage(RETURNVALUE_NOTPOSSIBLE);
			return;
		}

		item = thing->getItem();
	}

	if (item->getID() != itemId) {
		player->sendCancelMessage(RETURNVALUE_NOTPOSSIBLE);
		return;
	}

	std::shared_ptr<Cylinder> fromCylinder = nullptr;
	if (fromPos.x == 0xFFFF && (fromPos.y == 0x20 || fromPos.y == 0x21)) {
		// '0x20' -> From depot.
		// '0x21' -> From inbox.
		// Both only when the item is being moved from depot search window.
		if (!player->isDepotSearchOpenOnItem(itemId)) {
			player->sendCancelMessage(RETURNVALUE_NOTPOSSIBLE);
			return;
		}

		fromCylinder = item->getParent();
	} else {
		fromCylinder = internalGetCylinder(player, fromPos);
	}

	if (fromCylinder == nullptr) {
		player->sendCancelMessage(RETURNVALUE_NOTPOSSIBLE);
		return;
	}

	if (toCylinder == nullptr) {
		toCylinder = internalGetCylinder(player, toPos);
		if (toCylinder == nullptr) {
			player->sendCancelMessage(RETURNVALUE_NOTPOSSIBLE);
			return;
		}
	}

	// check if we can move this item
	if (ReturnValue ret = checkMoveItemToCylinder(player, fromCylinder, toCylinder, item, toPos); ret != RETURNVALUE_NOERROR) {
		player->sendCancelMessage(ret);
		return;
	}

	if (isTryingToStow(toPos, toCylinder)) {
		player->stowItem(item, count, false);
		return;
	}

	if (!item->isPushable() || item->hasAttribute(ItemAttribute_t::UNIQUEID)) {
		player->sendCancelMessage(RETURNVALUE_NOTMOVEABLE);
		return;
	}

	const Position &playerPos = player->getPosition();
	auto cylinderTile = fromCylinder->getTile();
	const Position &mapFromPos = cylinderTile ? cylinderTile->getPosition() : item->getPosition();
	if (playerPos.z != mapFromPos.z) {
		player->sendCancelMessage(playerPos.z > mapFromPos.z ? RETURNVALUE_FIRSTGOUPSTAIRS : RETURNVALUE_FIRSTGODOWNSTAIRS);
		return;
	}

	if (!Position::areInRange<1, 1>(playerPos, mapFromPos)) {
		// need to walk to the item first before using it
		stdext::arraylist<Direction> listDir(128);
		if (player->getPathTo(item->getPosition(), listDir, 0, 1, true, true)) {
			g_dispatcher().addEvent(std::bind(&Game::playerAutoWalk, this, player->getID(), listDir.data()), "Game::playerAutoWalk");

			std::shared_ptr<Task> task = createPlayerTask(400, std::bind(&Game::playerMoveItemByPlayerID, this, player->getID(), fromPos, itemId, fromStackPos, toPos, count), "Game::playerMoveItemByPlayerID");
			player->setNextWalkActionTask(task);
		} else {
			player->sendCancelMessage(RETURNVALUE_THEREISNOWAY);
		}
		return;
	}

	std::shared_ptr<Tile> toCylinderTile = toCylinder->getTile();
	const Position &mapToPos = toCylinderTile->getPosition();

	// hangable item specific code
	if (item->isHangable() && toCylinderTile->hasFlag(TILESTATE_SUPPORTS_HANGABLE)) {
		// destination supports hangable objects so need to move there first
		bool vertical = toCylinderTile->hasProperty(CONST_PROP_ISVERTICAL);
		if (vertical) {
			if (playerPos.x + 1 == mapToPos.x) {
				player->sendCancelMessage(RETURNVALUE_NOTPOSSIBLE);
				return;
			}
		} else { // horizontal
			if (playerPos.y + 1 == mapToPos.y) {
				player->sendCancelMessage(RETURNVALUE_NOTPOSSIBLE);
				return;
			}
		}

		if (!Position::areInRange<1, 1, 0>(playerPos, mapToPos)) {
			Position walkPos = mapToPos;
			if (vertical) {
				walkPos.x++;
			} else {
				walkPos.y++;
			}

			Position itemPos = fromPos;
			uint8_t itemStackPos = fromStackPos;

			if (fromPos.x != 0xFFFF && Position::areInRange<1, 1>(mapFromPos, playerPos)
				&& !Position::areInRange<1, 1, 0>(mapFromPos, walkPos)) {
				// need to pickup the item first
				std::shared_ptr<Item> moveItem = nullptr;

				ReturnValue ret = internalMoveItem(fromCylinder, player, INDEX_WHEREEVER, item, count, &moveItem);
				if (ret != RETURNVALUE_NOERROR) {
					player->sendCancelMessage(ret);
					return;
				}

				// changing the position since its now in the inventory of the player
				internalGetPosition(moveItem, itemPos, itemStackPos);
			}

			stdext::arraylist<Direction> listDir(128);
			if (player->getPathTo(walkPos, listDir, 0, 0, true, true)) {
				g_dispatcher().addEvent(std::bind(&Game::playerAutoWalk, this, player->getID(), listDir.data()), "Game::playerAutoWalk");

				std::shared_ptr<Task> task = createPlayerTask(400, std::bind(&Game::playerMoveItemByPlayerID, this, player->getID(), itemPos, itemId, itemStackPos, toPos, count), "Game::playerMoveItemByPlayerID");
				player->setNextWalkActionTask(task);
			} else {
				player->sendCancelMessage(RETURNVALUE_THEREISNOWAY);
			}
			return;
		}
	}

	auto throwRange = item->getThrowRange();
	if ((Position::getDistanceX(playerPos, mapToPos) > throwRange) || (Position::getDistanceY(playerPos, mapToPos) > throwRange) || (Position::getDistanceZ(mapFromPos, mapToPos) * 4 > throwRange)) {
		player->sendCancelMessage(RETURNVALUE_DESTINATIONOUTOFREACH);
		return;
	}

	if (!canThrowObjectTo(mapFromPos, mapToPos)) {
		player->sendCancelMessage(RETURNVALUE_CANNOTTHROW);
		return;
	}

	if (!g_callbacks().checkCallback(EventCallback_t::playerOnMoveItem, &EventCallback::playerOnMoveItem, player, item, count, fromPos, toPos, fromCylinder, toCylinder)) {
		return;
	}

	if (!g_events().eventPlayerOnMoveItem(player, item, count, fromPos, toPos, fromCylinder, toCylinder)) {
		return;
	}

	uint8_t toIndex = 0;
	if (toPos.x == 0xFFFF) {
		if (toPos.y & 0x40) {
			toIndex = toPos.z;
		} else {
			toIndex = static_cast<uint8_t>(toPos.y);
		}
	}

	if (item->isWrapable()) {
		auto toHouseTile = map.getTile(mapToPos)->dynamic_self_cast<HouseTile>();
		auto fromHouseTile = map.getTile(mapFromPos)->dynamic_self_cast<HouseTile>();
		if (fromHouseTile && (!toHouseTile || toHouseTile->getHouse()->getId() != fromHouseTile->getHouse()->getId())) {
			player->sendCancelMessage("You can't move this item outside a house.");
			return;
		}
	}

	ReturnValue ret = internalMoveItem(fromCylinder, toCylinder, toIndex, item, count, nullptr, 0, player);
	if (ret != RETURNVALUE_NOERROR) {
		player->sendCancelMessage(ret);
	} else if (toCylinder->getContainer() && fromCylinder->getContainer() && fromCylinder->getContainer()->countsToLootAnalyzerBalance() && toCylinder->getContainer()->getTopParent() == player) {
		player->sendLootStats(item, count);
	}
	player->cancelPush();

	item->checkDecayMapItemOnMove();

	g_events().eventPlayerOnItemMoved(player, item, count, fromPos, toPos, fromCylinder, toCylinder);
	g_callbacks().executeCallback(EventCallback_t::playerOnItemMoved, &EventCallback::playerOnItemMoved, player, item, count, fromPos, toPos, fromCylinder, toCylinder);
}

bool Game::isTryingToStow(const Position &toPos, std::shared_ptr<Cylinder> toCylinder) const {
	return toCylinder->getContainer() && toCylinder->getItem()->getID() == ITEM_LOCKER && toPos.getZ() == ITEM_SUPPLY_STASH_INDEX;
}

ReturnValue Game::checkMoveItemToCylinder(std::shared_ptr<Player> player, std::shared_ptr<Cylinder> fromCylinder, std::shared_ptr<Cylinder> toCylinder, std::shared_ptr<Item> item, Position toPos) {
	if (!player || !toCylinder || !item) {
		return RETURNVALUE_NOTPOSSIBLE;
	}

	if (std::shared_ptr<Container> toCylinderContainer = toCylinder->getContainer()) {
		auto containerID = toCylinderContainer->getID();

		// check the store inbox index if gold pouch forces it as containerID
		if (containerID == ITEM_STORE_INBOX) {
			auto cylinderItem = toCylinderContainer->getItemByIndex(toPos.getZ());
			if (cylinderItem && cylinderItem->getID() == ITEM_GOLD_POUCH) {
				containerID = ITEM_GOLD_POUCH;
			}
		}

		if (containerID == ITEM_GOLD_POUCH) {
			if (g_configManager().getBoolean(TOGGLE_GOLD_POUCH_QUICKLOOT_ONLY, __FUNCTION__)) {
				return RETURNVALUE_CONTAINERNOTENOUGHROOM;
			}

			bool allowAnything = g_configManager().getBoolean(TOGGLE_GOLD_POUCH_ALLOW_ANYTHING, __FUNCTION__);

			if (!allowAnything && item->getID() != ITEM_GOLD_COIN && item->getID() != ITEM_PLATINUM_COIN && item->getID() != ITEM_CRYSTAL_COIN) {
				return RETURNVALUE_CONTAINERNOTENOUGHROOM;
			}

			// prevent move up
			if (!item->isStoreItem() && fromCylinder->getContainer() && fromCylinder->getContainer()->getID() == ITEM_GOLD_POUCH) {
				return RETURNVALUE_CONTAINERNOTENOUGHROOM;
			}

			return RETURNVALUE_NOERROR;
		}

		std::shared_ptr<Container> topParentContainer = toCylinderContainer->getRootContainer();
		const auto parentContainer = topParentContainer->getParent() ? topParentContainer->getParent()->getContainer() : nullptr;
		auto isStoreInbox = parentContainer && parentContainer->isStoreInbox();
		if (!item->isStoreItem() && (containerID == ITEM_STORE_INBOX || isStoreInbox)) {
			return RETURNVALUE_CONTAINERNOTENOUGHROOM;
		}

		if (item->isStoreItem()) {
			bool isValidMoveItem = false;
			auto fromHouseTile = fromCylinder->getTile();
			auto house = fromHouseTile ? fromHouseTile->getHouse() : nullptr;
			if (house && house->getHouseAccessLevel(player) < HOUSE_OWNER) {
				return RETURNVALUE_NOTPOSSIBLE;
			}

			if (containerID == ITEM_STORE_INBOX || containerID == ITEM_DEPOT || toCylinderContainer->isDepotChest()) {
				isValidMoveItem = true;
			}

			if (parentContainer && (parentContainer->isDepotChest() || isStoreInbox)) {
				isValidMoveItem = true;
			}

			if (item->getID() == ITEM_GOLD_POUCH) {
				isValidMoveItem = true;
			}

			if (!isValidMoveItem) {
				return RETURNVALUE_NOTPOSSIBLE;
			}
		}

		if (item->getContainer() && !item->isStoreItem()) {
			for (std::shared_ptr<Item> containerItem : item->getContainer()->getItems(true)) {
				if (containerItem->isStoreItem() && ((containerID != ITEM_GOLD_POUCH && containerID != ITEM_DEPOT && containerID != ITEM_STORE_INBOX) || (topParentContainer->getParent() && topParentContainer->getParent()->getContainer() && (!topParentContainer->getParent()->getContainer()->isDepotChest() || topParentContainer->getParent()->getContainer()->getID() != ITEM_STORE_INBOX)))) {
					return RETURNVALUE_NOTPOSSIBLE;
				}
			}
		}
	} else if (toCylinder->getTile()) {
		const auto toHouseTile = toCylinder->getTile();
		auto house = toHouseTile ? toHouseTile->getHouse() : nullptr;
		if (fromCylinder->getContainer()) {
			if (item->isStoreItem()) {
				if (house && house->getHouseAccessLevel(player) < HOUSE_OWNER) {
					return RETURNVALUE_NOTPOSSIBLE;
				}
			}
			if (item->getContainer() && !item->isStoreItem()) {
				for (std::shared_ptr<Item> containerItem : item->getContainer()->getItems(true)) {
					if (containerItem->isStoreItem()) {
						return RETURNVALUE_NOTPOSSIBLE;
					}
				}
			}

			if (item->isStoreItem() && !house) {
				return RETURNVALUE_NOTPOSSIBLE;
			}
		}
	}

	return RETURNVALUE_NOERROR;
}

ReturnValue Game::internalMoveItem(std::shared_ptr<Cylinder> fromCylinder, std::shared_ptr<Cylinder> toCylinder, int32_t index, std::shared_ptr<Item> item, uint32_t count, std::shared_ptr<Item>* movedItem, uint32_t flags /*= 0*/, std::shared_ptr<Creature> actor /*=nullptr*/, std::shared_ptr<Item> tradeItem /* = nullptr*/, bool checkTile /* = true*/) {
	metrics::method_latency measure(__METHOD_NAME__);
	if (fromCylinder == nullptr) {
		g_logger().error("[{}] fromCylinder is nullptr", __FUNCTION__);
		return RETURNVALUE_NOTPOSSIBLE;
	}
	if (toCylinder == nullptr) {
		g_logger().error("[{}] toCylinder is nullptr", __FUNCTION__);
		return RETURNVALUE_NOTPOSSIBLE;
	}

	if (checkTile) {
		if (std::shared_ptr<Tile> fromTile = fromCylinder->getTile()) {
			if (fromTile && browseFields.contains(fromTile) && browseFields[fromTile].lock() == fromCylinder) {
				fromCylinder = fromTile;
			}
		}
	}

	std::shared_ptr<Item> toItem = nullptr;

	std::shared_ptr<Cylinder> subCylinder;
	int floorN = 0;

	while ((subCylinder = toCylinder->queryDestination(index, item, &toItem, flags)) != toCylinder) {
		toCylinder = subCylinder;
		flags = 0;

		// to prevent infinite loop
		if (++floorN >= MAP_MAX_LAYERS) {
			break;
		}
	}

	// destination is the same as the source?
	if (item == toItem) {
		return RETURNVALUE_NOERROR; // silently ignore move
	}

	// 'Move up' stackable items fix
	//  Cip's client never sends the count of stackables when using "Move up" menu option
	if (item->isStackable() && count == 255 && fromCylinder->getParent() == toCylinder) {
		count = item->getItemCount();
	}

	// check if we can add this item
	ReturnValue ret = toCylinder->queryAdd(index, item, count, flags, actor);
	if (ret == RETURNVALUE_NEEDEXCHANGE) {
		// check if we can add it to source cylinder
		ret = fromCylinder->queryAdd(fromCylinder->getThingIndex(item), toItem, toItem->getItemCount(), 0);
		if (ret == RETURNVALUE_NOERROR) {
			// check how much we can move
			uint32_t maxExchangeQueryCount = 0;
			ReturnValue retExchangeMaxCount = fromCylinder->queryMaxCount(INDEX_WHEREEVER, toItem, toItem->getItemCount(), maxExchangeQueryCount, 0);

			if (retExchangeMaxCount != RETURNVALUE_NOERROR && maxExchangeQueryCount == 0) {
				return retExchangeMaxCount;
			}

			if (toCylinder->queryRemove(toItem, toItem->getItemCount(), flags, actor) == RETURNVALUE_NOERROR) {
				int32_t oldToItemIndex = toCylinder->getThingIndex(toItem);
				toCylinder->removeThing(toItem, toItem->getItemCount());
				fromCylinder->addThing(toItem);

				if (oldToItemIndex != -1) {
					toCylinder->postRemoveNotification(toItem, fromCylinder, oldToItemIndex);
				}

				int32_t newToItemIndex = fromCylinder->getThingIndex(toItem);
				if (newToItemIndex != -1) {
					fromCylinder->postAddNotification(toItem, toCylinder, newToItemIndex);
				}

				ret = toCylinder->queryAdd(index, item, count, flags);
				toItem = nullptr;
			}
		}
	}

	if (ret != RETURNVALUE_NOERROR) {
		return ret;
	}

	// check how much we can move
	uint32_t maxQueryCount = 0;
	ReturnValue retMaxCount = toCylinder->queryMaxCount(index, item, count, maxQueryCount, flags);
	if (retMaxCount != RETURNVALUE_NOERROR && maxQueryCount == 0) {
		return retMaxCount;
	}

	uint32_t m;
	if (item->isStackable()) {
		m = std::min<uint32_t>(count, maxQueryCount);
	} else {
		m = maxQueryCount;
	}

	std::shared_ptr<Item> moveItem = item;
	// check if we can remove this item
	ret = fromCylinder->queryRemove(item, m, flags, actor);
	if (ret != RETURNVALUE_NOERROR) {
		return ret;
	}

	if (tradeItem) {
		if (toCylinder->getItem() == tradeItem) {
			return RETURNVALUE_NOTENOUGHROOM;
		}

		std::shared_ptr<Cylinder> tmpCylinder = toCylinder->getParent();
		while (tmpCylinder) {
			if (tmpCylinder->getItem() == tradeItem) {
				return RETURNVALUE_NOTENOUGHROOM;
			}

			tmpCylinder = tmpCylinder->getParent();
		}
	}

	// remove the item
	int32_t itemIndex = fromCylinder->getThingIndex(item);
	std::shared_ptr<Item> updateItem = nullptr;
	fromCylinder->removeThing(item, m);

	// update item(s)
	if (item->isStackable()) {
		uint32_t n;

		if (toItem && item->equals(toItem)) {
			n = std::min<uint32_t>(toItem->getStackSize() - toItem->getItemCount(), m);
			toCylinder->updateThing(toItem, toItem->getID(), toItem->getItemCount() + n);
			updateItem = toItem;
		} else {
			n = 0;
		}

		int32_t newCount = m - n;
		if (newCount > 0) {
			moveItem = item->clone();
			moveItem->setItemCount(newCount);
		} else {
			moveItem = nullptr;
		}

		if (item->isRemoved()) {
			item->stopDecaying();
		}
	}

	// add item
	if (moveItem /*m - n > 0*/) {
		toCylinder->addThing(index, moveItem);
	}

	if (itemIndex != -1) {
		fromCylinder->postRemoveNotification(item, toCylinder, itemIndex);
	}

	if (moveItem) {
		int32_t moveItemIndex = toCylinder->getThingIndex(moveItem);
		if (moveItemIndex != -1) {
			toCylinder->postAddNotification(moveItem, fromCylinder, moveItemIndex);
		}
		moveItem->startDecaying();
	}

	if (updateItem) {
		int32_t updateItemIndex = toCylinder->getThingIndex(updateItem);
		if (updateItemIndex != -1) {
			toCylinder->postAddNotification(updateItem, fromCylinder, updateItemIndex);
		}
		updateItem->startDecaying();
	}

	if (movedItem) {
		if (moveItem) {
			*movedItem = moveItem;
		} else {
			*movedItem = item;
		}
	}

	std::shared_ptr<Item> quiver = toCylinder->getItem();
	if (quiver && quiver->isQuiver()
		&& quiver->getHoldingPlayer()
		&& quiver->getHoldingPlayer()->getThing(CONST_SLOT_RIGHT) == quiver) {
		quiver->getHoldingPlayer()->sendInventoryItem(CONST_SLOT_RIGHT, quiver);
	} else {
		quiver = fromCylinder->getItem();
		if (quiver && quiver->isQuiver()
			&& quiver->getHoldingPlayer()
			&& quiver->getHoldingPlayer()->getThing(CONST_SLOT_RIGHT) == quiver) {
			quiver->getHoldingPlayer()->sendInventoryItem(CONST_SLOT_RIGHT, quiver);
		}
	}

	if (SoundEffect_t soundEffect = item->getMovementSound(toCylinder);
		toCylinder && soundEffect != SoundEffect_t::SILENCE) {
		if (toCylinder->getContainer() && actor && actor->getPlayer() && (toCylinder->getContainer()->isInsideDepot(true) || toCylinder->getContainer()->getHoldingPlayer())) {
			actor->getPlayer()->sendSingleSoundEffect(toCylinder->getPosition(), soundEffect, SourceEffect_t::OWN);
		} else {
			sendSingleSoundEffect(toCylinder->getPosition(), soundEffect, actor);
		}
	}

	// we could not move all, inform the player
	if (item->isStackable() && maxQueryCount < count) {
		return retMaxCount;
	}

	auto fromContainer = fromCylinder ? fromCylinder->getContainer() : nullptr;
	auto toContainer = toCylinder ? toCylinder->getContainer() : nullptr;
	auto player = actor ? actor->getPlayer() : nullptr;
	if (player) {
		// Update containers
		player->onSendContainer(toContainer);
		player->onSendContainer(fromContainer);
	}

	// Actor related actions
	if (fromCylinder && actor && toCylinder) {
		if (!fromContainer || !toContainer || !player) {
			return ret;
		}

		if (std::shared_ptr<Player> player = actor->getPlayer()) {
			// Refresh depot search window if necessary
			if (player->isDepotSearchOpenOnItem(item->getID()) && ((fromCylinder->getItem() && fromCylinder->getItem()->isInsideDepot(true)) || (toCylinder->getItem() && toCylinder->getItem()->isInsideDepot(true)))) {
				player->requestDepotSearchItem(item->getID(), item->getTier());
			}

			const ItemType &it = Item::items[fromCylinder->getItem()->getID()];
			if (it.id <= 0) {
				return ret;
			}

			// Looting analyser
			if (it.isCorpse && toContainer->getTopParent() == player && item->getIsLootTrackeable()) {
				player->sendLootStats(item, static_cast<uint8_t>(item->getItemCount()));
			}
		}
	}

	return ret;
}

ReturnValue Game::internalAddItem(std::shared_ptr<Cylinder> toCylinder, std::shared_ptr<Item> item, int32_t index /*= INDEX_WHEREEVER*/, uint32_t flags /* = 0*/, bool test /* = false*/) {
	uint32_t remainderCount = 0;
	return internalAddItem(toCylinder, item, index, flags, test, remainderCount);
}

ReturnValue Game::internalAddItem(std::shared_ptr<Cylinder> toCylinder, std::shared_ptr<Item> item, int32_t index, uint32_t flags, bool test, uint32_t &remainderCount) {
	metrics::method_latency measure(__METHOD_NAME__);
	if (toCylinder == nullptr) {
		g_logger().error("[{}] fromCylinder is nullptr", __FUNCTION__);
		return RETURNVALUE_NOTPOSSIBLE;
	}
	if (item == nullptr) {
		g_logger().error("[{}] item is nullptr", __FUNCTION__);
		return RETURNVALUE_NOTPOSSIBLE;
	}

	auto addedItem = toCylinder->getItem();

	std::shared_ptr<Cylinder> destCylinder = toCylinder;
	std::shared_ptr<Item> toItem = nullptr;
	toCylinder = toCylinder->queryDestination(index, item, &toItem, flags);

	// check if we can add this item
	ReturnValue ret = toCylinder->queryAdd(index, item, item->getItemCount(), flags);
	if (ret != RETURNVALUE_NOERROR) {
		return ret;
	}

	/*
	Check if we can move add the whole amount, we do this by checking against the original cylinder,
	since the queryDestination can return a cylinder that might only hold a part of the full amount.
	*/
	uint32_t maxQueryCount = 0;
	ret = destCylinder->queryMaxCount(INDEX_WHEREEVER, item, item->getItemCount(), maxQueryCount, flags);

	if (ret != RETURNVALUE_NOERROR && addedItem && addedItem->getID() != ITEM_REWARD_CONTAINER) {
		return ret;
	}

	if (test) {
		return RETURNVALUE_NOERROR;
	}

	if (item->isStackable() && item->equals(toItem)) {
		uint32_t m = std::min<uint32_t>(item->getItemCount(), maxQueryCount);
		uint32_t n = std::min<uint32_t>(toItem->getStackSize() - toItem->getItemCount(), m);

		toCylinder->updateThing(toItem, toItem->getID(), toItem->getItemCount() + n);

		int32_t count = m - n;
		if (count > 0) {
			if (item->getItemCount() != count) {
				std::shared_ptr<Item> remainderItem = item->clone();
				remainderItem->setItemCount(count);
				if (internalAddItem(destCylinder, remainderItem, INDEX_WHEREEVER, flags, false) != RETURNVALUE_NOERROR) {
					remainderCount = count;
				}
			} else {
				toCylinder->addThing(index, item);

				int32_t itemIndex = toCylinder->getThingIndex(item);
				if (itemIndex != -1) {
					toCylinder->postAddNotification(item, nullptr, itemIndex);
				}
			}
		} else {
			// fully merged with toItem, item will be destroyed
			item->onRemoved();

			int32_t itemIndex = toCylinder->getThingIndex(toItem);
			if (itemIndex != -1) {
				toCylinder->postAddNotification(toItem, nullptr, itemIndex);
			}
		}
	} else {
		toCylinder->addThing(index, item);

		int32_t itemIndex = toCylinder->getThingIndex(item);
		if (itemIndex != -1) {
			toCylinder->postAddNotification(item, nullptr, itemIndex);
		}
	}

	if (addedItem && addedItem->isQuiver()
		&& addedItem->getHoldingPlayer()
		&& addedItem->getHoldingPlayer()->getThing(CONST_SLOT_RIGHT) == addedItem) {
		addedItem->getHoldingPlayer()->sendInventoryItem(CONST_SLOT_RIGHT, addedItem);
	}

	return RETURNVALUE_NOERROR;
}

ReturnValue Game::internalRemoveItem(std::shared_ptr<Item> item, int32_t count /*= -1*/, bool test /*= false*/, uint32_t flags /*= 0*/, bool force /*= false*/) {
	metrics::method_latency measure(__METHOD_NAME__);
	if (item == nullptr) {
		g_logger().debug("{} - Item is nullptr", __FUNCTION__);
		return RETURNVALUE_NOTPOSSIBLE;
	}
	std::shared_ptr<Cylinder> cylinder = item->getParent();
	if (cylinder == nullptr) {
		g_logger().debug("{} - Cylinder is nullptr", __FUNCTION__);
		return RETURNVALUE_NOTPOSSIBLE;
	}
	std::shared_ptr<Tile> fromTile = cylinder->getTile();
	if (fromTile) {
		if (fromTile && browseFields.contains(fromTile) && browseFields[fromTile].lock() == cylinder) {
			cylinder = fromTile;
		}
	}
	if (count == -1) {
		count = item->getItemCount();
	}
	ReturnValue ret = cylinder->queryRemove(item, count, flags | FLAG_IGNORENOTMOVEABLE);
	if (!force && ret != RETURNVALUE_NOERROR) {
		g_logger().debug("{} - Failed to execute query remove", __FUNCTION__);
		return ret;
	}
	if (!force && !item->canRemove()) {
		g_logger().debug("{} - Failed to remove item", __FUNCTION__);
		return RETURNVALUE_NOTPOSSIBLE;
	}

	// Not remove item with decay loaded from map
	if (!force && item->canDecay() && cylinder->getTile() && item->isLoadedFromMap()) {
		g_logger().debug("Cannot remove item with id {}, name {}, on position {}", item->getID(), item->getName(), cylinder->getPosition().toString());
		item->stopDecaying();
		return RETURNVALUE_THISISIMPOSSIBLE;
	}

	if (!test) {
		int32_t index = cylinder->getThingIndex(item);
		// remove the item
		cylinder->removeThing(item, count);

		if (item->isRemoved()) {
			item->onRemoved();
			item->stopDecaying();
		}

		cylinder->postRemoveNotification(item, nullptr, index);
	}

	std::shared_ptr<Item> quiver = cylinder->getItem();
	if (quiver && quiver->isQuiver()
		&& quiver->getHoldingPlayer()
		&& quiver->getHoldingPlayer()->getThing(CONST_SLOT_RIGHT) == quiver) {
		quiver->getHoldingPlayer()->sendInventoryItem(CONST_SLOT_RIGHT, quiver);
	}

	return RETURNVALUE_NOERROR;
}

std::tuple<ReturnValue, uint32_t, uint32_t> Game::addItemBatch(const std::shared_ptr<Cylinder> &toCylinder, const std::vector<std::shared_ptr<Item>> &items, uint32_t flags /* = 0 */, bool dropOnMap /* = true */, uint32_t autoContainerId /* = 0 */) {
	metrics::method_latency measure(__METHOD_NAME__);
	const auto player = toCylinder->getPlayer();
	bool dropping = false;
	ReturnValue ret = RETURNVALUE_NOTPOSSIBLE;
	uint32_t totalAdded = 0;
	uint32_t containersCreated = 0;

	auto setupDestination = [&]() -> std::shared_ptr<Cylinder> {
		if (autoContainerId == 0) {
			return toCylinder;
		}
		auto autoContainer = Item::CreateItem(autoContainerId);
		if (!autoContainer) {
			g_logger().error("[{}] Failed to create auto container", __FUNCTION__);
			return toCylinder;
		}
		if (internalAddItem(toCylinder, autoContainer, CONST_SLOT_WHEREEVER, flags) != RETURNVALUE_NOERROR) {
			if (internalAddItem(toCylinder->getTile(), autoContainer, INDEX_WHEREEVER, FLAG_NOLIMIT) != RETURNVALUE_NOERROR) {
				g_logger().error("[{}] Failed to add auto container", __FUNCTION__);
				return toCylinder;
			}
		}
		auto container = autoContainer->getContainer();
		if (!container) {
			g_logger().error("[{}] Failed to get auto container", __FUNCTION__);
			return toCylinder;
		}
		containersCreated++;
		return container;
	};
	auto destination = setupDestination();

	for (const auto &item : items) {
		auto container = destination->getContainer();
		if (container && container->getFreeSlots() == 0) {
			destination = setupDestination();
		}
		if (!dropping) {
			uint32_t remainderCount = 0;
			ret = internalAddItem(destination, item, CONST_SLOT_WHEREEVER, flags, false, remainderCount);
			if (remainderCount != 0) {
				std::shared_ptr<Item> remainderItem = Item::CreateItem(item->getID(), remainderCount);
				ReturnValue remaindRet = internalAddItem(destination->getTile(), remainderItem, INDEX_WHEREEVER, FLAG_NOLIMIT);
				if (player && remaindRet != RETURNVALUE_NOERROR) {
					player->sendLootStats(item, static_cast<uint8_t>(item->getItemCount()));
				}
			}
		}

		if (dropping || ret != RETURNVALUE_NOERROR && dropOnMap) {
			dropping = true;
			ret = internalAddItem(destination->getTile(), item, INDEX_WHEREEVER, FLAG_NOLIMIT);
		}

		if (player && ret == RETURNVALUE_NOERROR) {
			player->sendForgingData();
		}
		if (ret != RETURNVALUE_NOERROR) {
			break;
		} else {
			totalAdded += item->getItemCount();
		}
	}

	return std::make_tuple(ret, totalAdded, containersCreated);
}

std::tuple<ReturnValue, uint32_t, uint32_t> Game::createItemBatch(const std::shared_ptr<Cylinder> &toCylinder, const std::vector<std::tuple<uint16_t, uint32_t, uint16_t>> &itemCounts, uint32_t flags /* = 0 */, bool dropOnMap /* = true */, uint32_t autoContainerId /* = 0 */) {
	metrics::method_latency measure(__METHOD_NAME__);
	std::vector<std::shared_ptr<Item>> items;
	for (const auto &[itemId, count, subType] : itemCounts) {
		const auto &itemType = Item::items[itemId];
		if (itemType.id <= 0) {
			continue;
		}
		if (count == 0) {
			continue;
		}
		uint32_t countPerItem = itemType.stackable ? itemType.stackSize : 1;
		for (uint32_t i = 0; i < count; ++i) {
			std::shared_ptr<Item> item;
			if (itemType.isWrappable()) {
				countPerItem = 1;
				item = Item::CreateItem(ITEM_DECORATION_KIT, subType);
				item->setAttribute(ItemAttribute_t::DESCRIPTION, "Unwrap this item in your own house to create a <" + itemType.name + ">.");
				item->setCustomAttribute("unWrapId", static_cast<int64_t>(itemId));
			} else {
				item = Item::CreateItem(itemId, itemType.stackable ? std::min<uint32_t>(countPerItem, count - i) : subType);
			}
			items.push_back(item);
			i += countPerItem - 1;
		}
	}

	return addItemBatch(toCylinder, items, flags, dropOnMap, autoContainerId);
}

std::tuple<ReturnValue, uint32_t, uint32_t> Game::createItem(const std::shared_ptr<Cylinder> &toCylinder, uint16_t itemId, uint32_t count, uint16_t subType, uint32_t flags /* = 0 */, bool dropOnMap /* = true */, uint32_t autoContainerId /* = 0 */) {
	return createItemBatch(toCylinder, { std::make_tuple(itemId, count, subType) }, flags, dropOnMap, autoContainerId);
}

ReturnValue Game::internalPlayerAddItem(std::shared_ptr<Player> player, std::shared_ptr<Item> item, bool dropOnMap /*= true*/, Slots_t slot /*= CONST_SLOT_WHEREEVER*/) {
	metrics::method_latency measure(__METHOD_NAME__);
	uint32_t remainderCount = 0;
	ReturnValue ret = internalAddItem(player, item, static_cast<int32_t>(slot), 0, false, remainderCount);
	if (remainderCount != 0) {
		std::shared_ptr<Item> remainderItem = Item::CreateItem(item->getID(), remainderCount);
		ReturnValue remaindRet = internalAddItem(player->getTile(), remainderItem, INDEX_WHEREEVER, FLAG_NOLIMIT);
		if (remaindRet != RETURNVALUE_NOERROR) {
			player->sendLootStats(item, static_cast<uint8_t>(item->getItemCount()));
		}
	}

	if (ret != RETURNVALUE_NOERROR && dropOnMap) {
		ret = internalAddItem(player->getTile(), item, INDEX_WHEREEVER, FLAG_NOLIMIT);
	}

	if (ret == RETURNVALUE_NOERROR) {
		player->sendForgingData();
	}

	return ret;
}

std::shared_ptr<Item> Game::findItemOfType(std::shared_ptr<Cylinder> cylinder, uint16_t itemId, bool depthSearch /*= true*/, int32_t subType /*= -1*/) const {
	metrics::method_latency measure(__METHOD_NAME__);
	if (cylinder == nullptr) {
		g_logger().error("[{}] Cylinder is nullptr", __FUNCTION__);
		return nullptr;
	}

	std::vector<std::shared_ptr<Container>> containers;
	for (size_t i = cylinder->getFirstIndex(), j = cylinder->getLastIndex(); i < j; ++i) {
		std::shared_ptr<Thing> thing = cylinder->getThing(i);
		if (!thing) {
			continue;
		}

		std::shared_ptr<Item> item = thing->getItem();
		if (!item) {
			continue;
		}

		if (item->getID() == itemId && (subType == -1 || subType == item->getSubType())) {
			return item;
		}

		if (depthSearch) {
			std::shared_ptr<Container> container = item->getContainer();
			if (container) {
				containers.push_back(container);
			}
		}
	}

	size_t i = 0;
	while (i < containers.size()) {
		std::shared_ptr<Container> container = containers[i++];
		for (std::shared_ptr<Item> item : container->getItemList()) {
			if (item->getID() == itemId && (subType == -1 || subType == item->getSubType())) {
				return item;
			}

			std::shared_ptr<Container> subContainer = item->getContainer();
			if (subContainer) {
				containers.push_back(subContainer);
			}
		}
	}
	return nullptr;
}

bool Game::removeMoney(std::shared_ptr<Cylinder> cylinder, uint64_t money, uint32_t flags /*= 0*/, bool useBalance /*= false*/) {
	if (cylinder == nullptr) {
		g_logger().error("[{}] cylinder is nullptr", __FUNCTION__);
		return false;
	}
	if (money == 0) {
		return true;
	}
	std::vector<std::shared_ptr<Container>> containers;
	std::multimap<uint32_t, std::shared_ptr<Item>> moneyMap;
	uint64_t moneyCount = 0;
	for (size_t i = cylinder->getFirstIndex(), j = cylinder->getLastIndex(); i < j; ++i) {
		std::shared_ptr<Thing> thing = cylinder->getThing(i);
		if (!thing) {
			continue;
		}
		std::shared_ptr<Item> item = thing->getItem();
		if (!item) {
			continue;
		}
		std::shared_ptr<Container> container = item->getContainer();
		if (container) {
			containers.push_back(container);
		} else {
			const uint32_t worth = item->getWorth();
			if (worth != 0) {
				moneyCount += worth;
				moneyMap.emplace(worth, item);
			}
		}
	}
	size_t i = 0;
	while (i < containers.size()) {
		std::shared_ptr<Container> container = containers[i++];
		for (std::shared_ptr<Item> item : container->getItemList()) {
			std::shared_ptr<Container> tmpContainer = item->getContainer();
			if (tmpContainer) {
				containers.push_back(tmpContainer);
			} else {
				const uint32_t worth = item->getWorth();
				if (worth != 0) {
					moneyCount += worth;
					moneyMap.emplace(worth, item);
				}
			}
		}
	}

	std::shared_ptr<Player> player = useBalance ? std::dynamic_pointer_cast<Player>(cylinder) : nullptr;
	uint64_t balance = 0;
	if (useBalance && player) {
		balance = player->getBankBalance();
	}

	if (moneyCount + balance < money) {
		return false;
	}

	for (const auto &moneyEntry : moneyMap) {
		std::shared_ptr<Item> item = moneyEntry.second;
		if (moneyEntry.first < money) {
			internalRemoveItem(item);
			money -= moneyEntry.first;
		} else if (moneyEntry.first > money) {
			const uint32_t worth = moneyEntry.first / item->getItemCount();
			const uint32_t removeCount = std::ceil(money / static_cast<double>(worth));
			addMoney(cylinder, (worth * removeCount) - money, flags);
			internalRemoveItem(item, removeCount);
			return true;
		} else {
			internalRemoveItem(item);
			return true;
		}
	}

	if (useBalance && player && player->getBankBalance() >= money) {
		player->setBankBalance(player->getBankBalance() - money);
	}

	return true;
}

void Game::addMoney(std::shared_ptr<Cylinder> cylinder, uint64_t money, uint32_t flags /*= 0*/) {
	if (cylinder == nullptr) {
		g_logger().error("[{}] cylinder is nullptr", __FUNCTION__);
		return;
	}
	if (money == 0) {
		return;
	}

	uint32_t crystalCoins = money / 10000;
	money -= crystalCoins * 10000;
	while (crystalCoins > 0) {
		const uint16_t count = std::min<uint32_t>(100, crystalCoins);

		std::shared_ptr<Item> remaindItem = Item::CreateItem(ITEM_CRYSTAL_COIN, count);

		ReturnValue ret = internalAddItem(cylinder, remaindItem, INDEX_WHEREEVER, flags);
		if (ret != RETURNVALUE_NOERROR) {
			internalAddItem(cylinder->getTile(), remaindItem, INDEX_WHEREEVER, FLAG_NOLIMIT);
		}

		crystalCoins -= count;
	}

	uint16_t platinumCoins = money / 100;
	if (platinumCoins != 0) {
		std::shared_ptr<Item> remaindItem = Item::CreateItem(ITEM_PLATINUM_COIN, platinumCoins);

		ReturnValue ret = internalAddItem(cylinder, remaindItem, INDEX_WHEREEVER, flags);
		if (ret != RETURNVALUE_NOERROR) {
			internalAddItem(cylinder->getTile(), remaindItem, INDEX_WHEREEVER, FLAG_NOLIMIT);
		}

		money -= platinumCoins * 100;
	}

	if (money != 0) {
		std::shared_ptr<Item> remaindItem = Item::CreateItem(ITEM_GOLD_COIN, money);

		ReturnValue ret = internalAddItem(cylinder, remaindItem, INDEX_WHEREEVER, flags);
		if (ret != RETURNVALUE_NOERROR) {
			internalAddItem(cylinder->getTile(), remaindItem, INDEX_WHEREEVER, FLAG_NOLIMIT);
		}
	}
}

std::shared_ptr<Item> Game::transformItem(std::shared_ptr<Item> item, uint16_t newId, int32_t newCount /*= -1*/) {
	metrics::method_latency measure(__METHOD_NAME__);
	if (item->getID() == newId && (newCount == -1 || (newCount == item->getSubType() && newCount != 0))) { // chargeless item placed on map = infinite
		return item;
	}

	std::shared_ptr<Cylinder> cylinder = item->getParent();
	if (cylinder == nullptr) {
		return nullptr;
	}

	std::shared_ptr<Tile> fromTile = cylinder->getTile();
	if (fromTile && browseFields.contains(fromTile) && browseFields[fromTile].lock() == cylinder) {
		cylinder = fromTile;
	}

	int32_t itemIndex = cylinder->getThingIndex(item);
	if (itemIndex == -1) {
		return item;
	}

	if (!item->canTransform()) {
		return item;
	}

	const ItemType &newType = Item::items[newId];
	if (newType.id == 0) {
		return item;
	}

	const ItemType &curType = Item::items[item->getID()];
	if (item->isAlwaysOnTop() != (newType.alwaysOnTopOrder != 0)) {
		// This only occurs when you transform items on tiles from a downItem to a topItem (or vice versa)
		// Remove the old, and add the new
		cylinder->removeThing(item, item->getItemCount());
		cylinder->postRemoveNotification(item, cylinder, itemIndex);

		item->setID(newId);
		if (newCount != -1) {
			item->setSubType(newCount);
		}
		cylinder->addThing(item);

		std::shared_ptr<Cylinder> newParent = item->getParent();
		if (newParent == nullptr) {
			item->stopDecaying();
			return nullptr;
		}

		newParent->postAddNotification(item, cylinder, newParent->getThingIndex(item));
		item->startDecaying();

		return item;
	}

	if (curType.type == newType.type) {
		// Both items has the same type so we can safely change id/subtype
		if (newCount == 0 && (item->isStackable() || item->hasAttribute(ItemAttribute_t::CHARGES))) {
			if (item->isStackable()) {
				internalRemoveItem(item);
				return nullptr;
			} else {
				int32_t newItemId = newId;
				if (curType.id == newType.id) {
					newItemId = curType.decayTo;
				}

				if (newItemId < 0) {
					internalRemoveItem(item);
					return nullptr;
				} else if (newItemId != newId) {
					// Replacing the the old item with the std::make_shared< while> maintaining the old position
					auto newItem = item->transform(newItemId);
					if (newItem == nullptr) {
						g_logger().error("[{}] new item with id {} is nullptr, (ERROR CODE: 01)", __FUNCTION__, newItemId);
						return nullptr;
					}

					return newItem;
				} else {
					return transformItem(item, newItemId);
				}
			}
		} else {
			cylinder->postRemoveNotification(item, cylinder, itemIndex);
			uint16_t itemId = item->getID();
			int32_t count = item->getSubType();

			if (curType.id != newType.id) {
				if (newType.group != curType.group) {
					item->setDefaultSubtype();
				}

				itemId = newId;
			}

			if (newCount != -1 && newType.hasSubType()) {
				count = newCount;
			}

			cylinder->updateThing(item, itemId, count);
			cylinder->postAddNotification(item, cylinder, itemIndex);

			std::shared_ptr<Item> quiver = cylinder->getItem();
			if (quiver && quiver->isQuiver()
				&& quiver->getHoldingPlayer()
				&& quiver->getHoldingPlayer()->getThing(CONST_SLOT_RIGHT) == quiver) {
				quiver->getHoldingPlayer()->sendInventoryItem(CONST_SLOT_RIGHT, quiver);
			}
			item->startDecaying();

			return item;
		}
	}

	std::shared_ptr<Item> quiver = cylinder->getItem();
	if (quiver && quiver->isQuiver()
		&& quiver->getHoldingPlayer()
		&& quiver->getHoldingPlayer()->getThing(CONST_SLOT_RIGHT) == quiver) {
		quiver->getHoldingPlayer()->sendInventoryItem(CONST_SLOT_RIGHT, quiver);
	}

	// Replacing the the old item with the new while maintaining the old position
	auto newItem = item->transform(newId, newCount);
	if (newItem == nullptr) {
		g_logger().error("[{}] new item with id {} is nullptr (ERROR CODE: 02)", __FUNCTION__, newId);
		return nullptr;
	}

	return newItem;
}

ReturnValue Game::internalTeleport(const std::shared_ptr<Thing> &thing, const Position &newPos, bool pushMove /* = true*/, uint32_t flags /*= 0*/) {
	metrics::method_latency measure(__METHOD_NAME__);
	if (thing == nullptr) {
		g_logger().error("[{}] thing is nullptr", __FUNCTION__);
		return RETURNVALUE_NOTPOSSIBLE;
	}

	if (newPos == thing->getPosition()) {
		return RETURNVALUE_CONTACTADMINISTRATOR;
	} else if (thing->isRemoved()) {
		return RETURNVALUE_NOTPOSSIBLE;
	}

	std::shared_ptr<Tile> toTile = map.getTile(newPos);
	if (!toTile) {
		return RETURNVALUE_NOTPOSSIBLE;
	}

	if (std::shared_ptr<Creature> creature = thing->getCreature()) {
		ReturnValue ret = toTile->queryAdd(0, creature, 1, FLAG_NOLIMIT);
		if (ret != RETURNVALUE_NOERROR) {
			return ret;
		}

		map.moveCreature(creature, toTile, !pushMove);
		return RETURNVALUE_NOERROR;
	} else if (std::shared_ptr<Item> item = thing->getItem()) {
		return internalMoveItem(item->getParent(), toTile, INDEX_WHEREEVER, item, item->getItemCount(), nullptr, flags);
	}
	return RETURNVALUE_NOTPOSSIBLE;
}

void Game::internalQuickLootCorpse(std::shared_ptr<Player> player, std::shared_ptr<Container> corpse) {
	if (!player || !corpse) {
		return;
	}

	std::vector<std::shared_ptr<Item>> itemList;
	bool ignoreListItems = (player->quickLootFilter == QUICKLOOTFILTER_SKIPPEDLOOT);

	bool missedAnyGold = false;
	bool missedAnyItem = false;

	for (ContainerIterator it = corpse->iterator(); it.hasNext(); it.advance()) {
		std::shared_ptr<Item> item = *it;
		bool listed = player->isQuickLootListedItem(item);
		if ((listed && ignoreListItems) || (!listed && !ignoreListItems)) {
			if (item->getWorth() != 0) {
				missedAnyGold = true;
			} else {
				missedAnyItem = true;
			}
			continue;
		}

		itemList.push_back(item);
	}

	bool shouldNotifyCapacity = false;
	ObjectCategory_t shouldNotifyNotEnoughRoom = OBJECTCATEGORY_NONE;

	uint32_t totalLootedGold = 0;
	uint32_t totalLootedItems = 0;
	for (std::shared_ptr<Item> item : itemList) {
		uint32_t worth = item->getWorth();
		uint16_t baseCount = item->getItemCount();
		ObjectCategory_t category = getObjectCategory(item);

		ReturnValue ret = internalCollectLootItems(player, item, category);
		if (ret == RETURNVALUE_NOTENOUGHCAPACITY) {
			shouldNotifyCapacity = true;
		} else if (ret == RETURNVALUE_CONTAINERNOTENOUGHROOM) {
			shouldNotifyNotEnoughRoom = category;
		}

		bool success = ret == RETURNVALUE_NOERROR;
		if (worth != 0) {
			missedAnyGold = missedAnyGold || !success;
			if (success) {
				player->sendLootStats(item, baseCount);
				totalLootedGold += worth;
			} else {
				// item is not completely moved
				totalLootedGold += worth - item->getWorth();
			}
		} else {
			missedAnyItem = missedAnyItem || !success;
			if (success || item->getItemCount() != baseCount) {
				totalLootedItems++;
				player->sendLootStats(item, item->getItemCount());
			}
		}
	}

	std::stringstream ss;
	if (totalLootedGold != 0 || missedAnyGold || totalLootedItems != 0 || missedAnyItem) {
		bool lootedAllGold = totalLootedGold != 0 && !missedAnyGold;
		bool lootedAllItems = totalLootedItems != 0 && !missedAnyItem;
		if (lootedAllGold) {
			if (totalLootedItems != 0 || missedAnyItem) {
				ss << "You looted the complete " << totalLootedGold << " gold";

				if (lootedAllItems) {
					ss << " and all dropped items";
				} else if (totalLootedItems != 0) {
					ss << ", but you only looted some of the items";
				} else if (missedAnyItem) {
					ss << " but none of the dropped items";
				}
			} else {
				ss << "You looted " << totalLootedGold << " gold";
			}
		} else if (lootedAllItems) {
			if (totalLootedItems == 1) {
				ss << "You looted 1 item";
			} else if (totalLootedGold != 0 || missedAnyGold) {
				ss << "You looted all of the dropped items";
			} else {
				ss << "You looted all items";
			}

			if (totalLootedGold != 0) {
				ss << ", but you only looted " << totalLootedGold << " of the dropped gold";
			} else if (missedAnyGold) {
				ss << " but none of the dropped gold";
			}
		} else if (totalLootedGold != 0) {
			ss << "You only looted " << totalLootedGold << " of the dropped gold";
			if (totalLootedItems != 0) {
				ss << " and some of the dropped items";
			} else if (missedAnyItem) {
				ss << " but none of the dropped items";
			}
		} else if (totalLootedItems != 0) {
			ss << "You looted some of the dropped items";
			if (missedAnyGold) {
				ss << " but none of the dropped gold";
			}
		} else if (missedAnyGold) {
			ss << "You looted none of the dropped gold";
			if (missedAnyItem) {
				ss << " and none of the items";
			}
		} else if (missedAnyItem) {
			ss << "You looted none of the dropped items";
		}
	} else {
		ss << "No loot";
	}

	if (player->checkAutoLoot()) {
		ss << " (automatic looting)";
	}
	ss << ".";
	player->sendTextMessage(MESSAGE_STATUS, ss.str());

	if (shouldNotifyCapacity) {
		ss.str(std::string());
		ss << "Attention! The loot you are trying to pick up is too heavy for you to carry.";
	} else if (shouldNotifyNotEnoughRoom != OBJECTCATEGORY_NONE) {
		ss.str(std::string());
		ss << "Attention! The container assigned to category " << getObjectCategoryName(shouldNotifyNotEnoughRoom) << " is full.";
	} else {
		return;
	}

	if (player->lastQuickLootNotification + 15000 < OTSYS_TIME()) {
		player->sendTextMessage(MESSAGE_GAME_HIGHLIGHT, ss.str());
	} else {
		player->sendTextMessage(MESSAGE_EVENT_ADVANCE, ss.str());
	}

	player->lastQuickLootNotification = OTSYS_TIME();
}

std::shared_ptr<Container> Game::findLootContainer(std::shared_ptr<Player> player, bool &fallbackConsumed, ObjectCategory_t category) {
	auto lootContainer = player->getLootContainer(category);
	if (!lootContainer && player->quickLootFallbackToMainContainer && !fallbackConsumed) {
		auto fallbackItem = player->getInventoryItem(CONST_SLOT_BACKPACK);
		auto mainBackpack = fallbackItem ? fallbackItem->getContainer() : nullptr;

		if (mainBackpack) {
			player->setLootContainer(OBJECTCATEGORY_DEFAULT, mainBackpack);
			player->sendInventoryItem(CONST_SLOT_BACKPACK, player->getInventoryItem(CONST_SLOT_BACKPACK));
			lootContainer = mainBackpack;
			fallbackConsumed = true;
		}
	}

	return lootContainer;
}

std::shared_ptr<Container> Game::findNextAvailableContainer(ContainerIterator &containerIterator, std::shared_ptr<Container> &lootContainer, std::shared_ptr<Container> &lastSubContainer) {
	while (containerIterator.hasNext()) {
		std::shared_ptr<Item> cur = *containerIterator;
		std::shared_ptr<Container> subContainer = cur ? cur->getContainer() : nullptr;
		containerIterator.advance();

		if (subContainer) {
			lastSubContainer = subContainer;
			lootContainer = subContainer;
			return lootContainer;
		}
	}

	// Fix last empty sub-container
	if (lastSubContainer && !lastSubContainer->empty()) {
		auto cur = lastSubContainer->getItemByIndex(lastSubContainer->size() - 1);
		lootContainer = cur ? cur->getContainer() : nullptr;
		lastSubContainer = nullptr;
		return lootContainer;
	}

	return nullptr;
}

bool Game::handleFallbackLogic(std::shared_ptr<Player> player, std::shared_ptr<Container> &lootContainer, ContainerIterator &containerIterator, const bool &fallbackConsumed) {
	if (fallbackConsumed || !player->quickLootFallbackToMainContainer) {
		return false;
	}

	std::shared_ptr<Item> fallbackItem = player->getInventoryItem(CONST_SLOT_BACKPACK);
	if (!fallbackItem || !fallbackItem->getContainer()) {
		return false;
	}

	lootContainer = fallbackItem->getContainer();
	containerIterator = lootContainer->iterator();

	return true;
}

ReturnValue Game::processMoveOrAddItemToLootContainer(std::shared_ptr<Item> item, std::shared_ptr<Container> lootContainer, uint32_t &remainderCount, std::shared_ptr<Player> player) {
	std::shared_ptr<Item> moveItem = nullptr;
	ReturnValue ret;
	if (item->getParent()) {
		ret = internalMoveItem(item->getParent(), lootContainer, INDEX_WHEREEVER, item, item->getItemCount(), &moveItem, 0, player, nullptr, false);
	} else {
		ret = internalAddItem(lootContainer, item, INDEX_WHEREEVER);
	}
	if (moveItem) {
		remainderCount -= moveItem->getItemCount();
	}
	return ret;
}

ReturnValue Game::processLootItems(std::shared_ptr<Player> player, std::shared_ptr<Container> lootContainer, std::shared_ptr<Item> item, bool &fallbackConsumed) {
	std::shared_ptr<Container> lastSubContainer = nullptr;
	uint32_t remainderCount = item->getItemCount();
	ContainerIterator containerIterator = lootContainer->iterator();

	ReturnValue ret;
	do {
		ret = processMoveOrAddItemToLootContainer(item, lootContainer, remainderCount, player);
		if (ret != RETURNVALUE_CONTAINERNOTENOUGHROOM) {
			return ret;
		}

		std::shared_ptr<Container> nextContainer = findNextAvailableContainer(containerIterator, lootContainer, lastSubContainer);
		if (!nextContainer && !handleFallbackLogic(player, lootContainer, containerIterator, fallbackConsumed)) {
			break;
		}
		fallbackConsumed = fallbackConsumed || (nextContainer == nullptr);
	} while (remainderCount != 0);

	return ret;
}

ReturnValue Game::internalCollectLootItems(std::shared_ptr<Player> player, std::shared_ptr<Item> item, ObjectCategory_t category /* = OBJECTCATEGORY_DEFAULT*/) {
	if (!player || !item) {
		return RETURNVALUE_NOTPOSSIBLE;
	}

	// Send money to the bank
	if (g_configManager().getBoolean(AUTOBANK, __FUNCTION__)) {
		if (item->getID() == ITEM_GOLD_COIN || item->getID() == ITEM_PLATINUM_COIN || item->getID() == ITEM_CRYSTAL_COIN) {
			uint64_t money = 0;
			if (item->getID() == ITEM_PLATINUM_COIN) {
				money = item->getItemCount() * 100;
			} else if (item->getID() == ITEM_CRYSTAL_COIN) {
				money = item->getItemCount() * 10000;
			} else {
				money = item->getItemCount();
			}
			auto parent = item->getParent();
			if (parent) {
				parent->removeThing(item, item->getItemCount());
			} else {
				g_logger().debug("Item has no parent");
				return RETURNVALUE_NOTPOSSIBLE;
			}
			player->setBankBalance(player->getBankBalance() + money);
			g_metrics().addCounter("balance_increase", money, { { "player", player->getName() }, { "context", "loot" } });
			return RETURNVALUE_NOERROR;
		}
	}

	bool fallbackConsumed = false;
	std::shared_ptr<Container> lootContainer = findLootContainer(player, fallbackConsumed, category);
	if (!lootContainer) {
		return RETURNVALUE_NOTPOSSIBLE;
	}

	return processLootItems(player, lootContainer, item, fallbackConsumed);
}

ReturnValue Game::collectRewardChestItems(std::shared_ptr<Player> player, uint32_t maxMoveItems /* = 0*/) {
	// Check if have item on player reward chest
	std::shared_ptr<RewardChest> rewardChest = player->getRewardChest();
	if (rewardChest->empty()) {
		g_logger().debug("Reward chest is empty");
		return RETURNVALUE_REWARDCHESTISEMPTY;
	}

	auto rewardItemsVector = player->getRewardsFromContainer(rewardChest->getContainer());
	auto rewardCount = rewardItemsVector.size();
	uint32_t movedRewardItems = 0;
	std::string lootedItemsMessage;
	for (auto item : rewardItemsVector) {
		// Stop if player not have free capacity
		if (item && player->getCapacity() < item->getWeight()) {
			player->sendCancelMessage(RETURNVALUE_NOTENOUGHCAPACITY);
			break;
		}

		// Limit the collect count if the "maxMoveItems" is not "0"
		auto limitMove = maxMoveItems != 0 && movedRewardItems == maxMoveItems;
		if (limitMove) {
			lootedItemsMessage = fmt::format("You can only collect {} items at a time. {} of {} objects were picked up.", maxMoveItems, movedRewardItems, rewardCount);
			player->sendTextMessage(MESSAGE_EVENT_ADVANCE, lootedItemsMessage);
			return RETURNVALUE_NOERROR;
		}

		ObjectCategory_t category = getObjectCategory(item);
		if (internalCollectLootItems(player, item, category) == RETURNVALUE_NOERROR) {
			movedRewardItems++;
		}
	}

	lootedItemsMessage = fmt::format("{} of {} objects were picked up.", movedRewardItems, rewardCount);
	player->sendTextMessage(MESSAGE_EVENT_ADVANCE, lootedItemsMessage);

	if (movedRewardItems == 0) {
		return RETURNVALUE_NOTPOSSIBLE;
	}

	return RETURNVALUE_NOERROR;
}

ObjectCategory_t Game::getObjectCategory(std::shared_ptr<Item> item) {
	ObjectCategory_t category = OBJECTCATEGORY_DEFAULT;
	if (!item) {
		return OBJECTCATEGORY_NONE;
	}

	const ItemType &it = Item::items[item->getID()];
	if (item->getWorth() != 0) {
		category = OBJECTCATEGORY_GOLD;
	} else if (it.weaponType != WEAPON_NONE) {
		switch (it.weaponType) {
			case WEAPON_SWORD:
				category = OBJECTCATEGORY_SWORDS;
				break;
			case WEAPON_CLUB:
				category = OBJECTCATEGORY_CLUBS;
				break;
			case WEAPON_AXE:
				category = OBJECTCATEGORY_AXES;
				break;
			case WEAPON_SHIELD:
				category = OBJECTCATEGORY_SHIELDS;
				break;
			case WEAPON_MISSILE:
			case WEAPON_DISTANCE:
				category = OBJECTCATEGORY_DISTANCEWEAPONS;
				break;
			case WEAPON_WAND:
				category = OBJECTCATEGORY_WANDS;
				break;
			case WEAPON_AMMO:
				category = OBJECTCATEGORY_AMMO;
				break;
			default:
				break;
		}
	} else if (it.slotPosition != SLOTP_HAND) { // if it's a weapon/shield should have been parsed earlier
		if ((it.slotPosition & SLOTP_HEAD) != 0) {
			category = OBJECTCATEGORY_HELMETS;
		} else if ((it.slotPosition & SLOTP_NECKLACE) != 0) {
			category = OBJECTCATEGORY_NECKLACES;
		} else if ((it.slotPosition & SLOTP_BACKPACK) != 0) {
			category = OBJECTCATEGORY_CONTAINERS;
		} else if ((it.slotPosition & SLOTP_ARMOR) != 0) {
			category = OBJECTCATEGORY_ARMORS;
		} else if ((it.slotPosition & SLOTP_LEGS) != 0) {
			category = OBJECTCATEGORY_LEGS;
		} else if ((it.slotPosition & SLOTP_FEET) != 0) {
			category = OBJECTCATEGORY_BOOTS;
		} else if ((it.slotPosition & SLOTP_RING) != 0) {
			category = OBJECTCATEGORY_RINGS;
		}
	} else if (it.type == ITEM_TYPE_RUNE) {
		category = OBJECTCATEGORY_RUNES;
	} else if (it.type == ITEM_TYPE_CREATUREPRODUCT) {
		category = OBJECTCATEGORY_CREATUREPRODUCTS;
	} else if (it.type == ITEM_TYPE_FOOD) {
		category = OBJECTCATEGORY_FOOD;
	} else if (it.type == ITEM_TYPE_VALUABLE) {
		category = OBJECTCATEGORY_VALUABLES;
	} else if (it.type == ITEM_TYPE_POTION) {
		category = OBJECTCATEGORY_POTIONS;
	} else {
		category = OBJECTCATEGORY_OTHERS;
	}

	return category;
}

uint64_t Game::getItemMarketPrice(const std::map<uint16_t, uint64_t> &itemMap, bool buyPrice) const {
	uint64_t total = 0;
	for (const auto &it : itemMap) {
		if (it.first == ITEM_GOLD_COIN) {
			total += it.second;
		} else if (it.first == ITEM_PLATINUM_COIN) {
			total += 100 * it.second;
		} else if (it.first == ITEM_CRYSTAL_COIN) {
			total += 10000 * it.second;
		} else {
			auto marketIt = itemsPriceMap.find(it.first);
			if (marketIt != itemsPriceMap.end()) {
				for (auto &[tier, price] : (*marketIt).second) {
					total += price * it.second;
				}
			} else {
				const ItemType &iType = Item::items[it.first];
				total += (buyPrice ? iType.buyPrice : iType.sellPrice) * it.second;
			}
		}
	}

	return total;
}

std::shared_ptr<Item> searchForItem(std::shared_ptr<Container> container, uint16_t itemId, bool hasTier /* = false*/, uint8_t tier /* = 0*/) {
	for (ContainerIterator it = container->iterator(); it.hasNext(); it.advance()) {
		if ((*it)->getID() == itemId && (!hasTier || (*it)->getTier() == tier)) {
			return *it;
		}
	}

	return nullptr;
}

Slots_t getSlotType(const ItemType &it) {
	Slots_t slot = CONST_SLOT_RIGHT;
	if (it.weaponType != WeaponType_t::WEAPON_SHIELD) {
		int32_t slotPosition = it.slotPosition;

		if (slotPosition & SLOTP_HEAD) {
			slot = CONST_SLOT_HEAD;
		} else if (slotPosition & SLOTP_NECKLACE) {
			slot = CONST_SLOT_NECKLACE;
		} else if (slotPosition & SLOTP_ARMOR) {
			slot = CONST_SLOT_ARMOR;
		} else if (slotPosition & SLOTP_LEGS) {
			slot = CONST_SLOT_LEGS;
		} else if (slotPosition & SLOTP_FEET) {
			slot = CONST_SLOT_FEET;
		} else if (slotPosition & SLOTP_RING) {
			slot = CONST_SLOT_RING;
		} else if (slotPosition & SLOTP_AMMO) {
			slot = CONST_SLOT_AMMO;
		} else if (slotPosition & SLOTP_TWO_HAND || slotPosition & SLOTP_LEFT) {
			slot = CONST_SLOT_LEFT;
		}
	}

	return slot;
}

// Implementation of player invoked events
void Game::playerEquipItem(uint32_t playerId, uint16_t itemId, bool hasTier /* = false*/, uint8_t tier /* = 0*/) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	if (player->hasCondition(CONDITION_FEARED)) {
		/*
		 *	When player is feared the player can´t equip any items.
		 */
		player->sendTextMessage(MESSAGE_FAILURE, "You are feared.");
		return;
	}

	std::shared_ptr<Item> item = player->getInventoryItem(CONST_SLOT_BACKPACK);
	if (!item) {
		return;
	}

	std::shared_ptr<Container> backpack = item->getContainer();
	if (!backpack) {
		return;
	}

	const ItemType &it = Item::items[itemId];
	Slots_t slot = getSlotType(it);

	auto slotItem = player->getInventoryItem(slot);
	auto equipItem = searchForItem(backpack, it.id, hasTier, tier);
	if (slotItem && slotItem->getID() == it.id && (!it.stackable || slotItem->getItemCount() == slotItem->getStackSize() || !equipItem)) {
		internalMoveItem(slotItem->getParent(), player, CONST_SLOT_WHEREEVER, slotItem, slotItem->getItemCount(), nullptr);
	} else if (equipItem) {
		if (it.weaponType == WEAPON_AMMO) {
			auto quiver = player->getInventoryItem(CONST_SLOT_RIGHT);
			if (quiver && quiver->isQuiver()) {
				internalMoveItem(equipItem->getParent(), quiver->getContainer(), 0, equipItem, equipItem->getItemCount(), nullptr);
				return;
			}
		}

		internalMoveItem(equipItem->getParent(), player, slot, equipItem, equipItem->getItemCount(), nullptr);
	}
}

void Game::playerMove(uint32_t playerId, Direction direction) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	player->resetIdleTime();
	player->setNextWalkActionTask(nullptr);
	player->cancelPush();

	player->startAutoWalk(std::vector<Direction> { direction }, false);
}

void Game::forcePlayerMove(uint32_t playerId, Direction direction) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	player->resetIdleTime();
	player->setNextWalkActionTask(nullptr);
	player->cancelPush();

	player->startAutoWalk(std::vector<Direction> { direction }, true);
}

bool Game::playerBroadcastMessage(std::shared_ptr<Player> player, const std::string &text) const {
	if (!player->hasFlag(PlayerFlags_t::CanBroadcast)) {
		return false;
	}

	g_logger().info("{} broadcasted: {}", player->getName(), text);

	for (const auto &it : players) {
		it.second->sendPrivateMessage(player, TALKTYPE_BROADCAST, text);
	}

	return true;
}

void Game::playerCreatePrivateChannel(uint32_t playerId) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player || !player->isPremium()) {
		return;
	}

	const auto &channel = g_chat().createChannel(player, CHANNEL_PRIVATE);
	if (!channel || !channel->addUser(player)) {
		return;
	}

	player->sendCreatePrivateChannel(channel->getId(), channel->getName());
}

void Game::playerChannelInvite(uint32_t playerId, const std::string &name) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	const auto &channel = g_chat().getPrivateChannel(player);
	if (!channel) {
		return;
	}

	std::shared_ptr<Player> invitePlayer = getPlayerByName(name);
	if (!invitePlayer) {
		return;
	}

	if (player == invitePlayer) {
		return;
	}

	channel->invitePlayer(player, invitePlayer);
}

void Game::playerChannelExclude(uint32_t playerId, const std::string &name) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	const auto &channel = g_chat().getPrivateChannel(player);
	if (!channel) {
		return;
	}

	std::shared_ptr<Player> excludePlayer = getPlayerByName(name);
	if (!excludePlayer) {
		return;
	}

	if (player == excludePlayer) {
		return;
	}

	channel->excludePlayer(player, excludePlayer);
}

void Game::playerRequestChannels(uint32_t playerId) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	player->sendChannelsDialog();
}

void Game::playerOpenChannel(uint32_t playerId, uint16_t channelId) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	const auto &channel = g_chat().addUserToChannel(player, channelId);
	if (!channel) {
		return;
	}

	const InvitedMap* invitedUsers = channel->getInvitedUsers();
	const UsersMap* users;
	if (!channel->isPublicChannel()) {
		users = &channel->getUsers();
	} else {
		users = nullptr;
	}

	player->sendChannel(channel->getId(), channel->getName(), users, invitedUsers);
}

void Game::playerCloseChannel(uint32_t playerId, uint16_t channelId) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	g_chat().removeUserFromChannel(player, channelId);
}

void Game::playerOpenPrivateChannel(uint32_t playerId, std::string &receiver) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	if (!IOLoginData::formatPlayerName(receiver)) {
		player->sendCancelMessage("A player with this name does not exist.");
		return;
	}

	if (player->getName() == receiver) {
		player->sendCancelMessage("You cannot set up a private message channel with yourself.");
		return;
	}

	player->sendOpenPrivateChannel(receiver);
}

void Game::playerCloseNpcChannel(uint32_t playerId) {
	const auto &player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	for (const auto &spectator : Spectators().find<Creature>(player->getPosition()).filter<Npc>()) {
		spectator->getNpc()->onPlayerCloseChannel(player);
	}
}

void Game::playerReceivePing(uint32_t playerId) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	player->receivePing();
}

void Game::playerReceivePingBack(uint32_t playerId) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	player->sendPingBack();
}

void Game::playerAutoWalk(uint32_t playerId, const std::vector<Direction> &listDir) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	player->resetIdleTime();
	player->setNextWalkTask(nullptr);
	player->startAutoWalk(listDir, false);
}

void Game::forcePlayerAutoWalk(uint32_t playerId, const std::vector<Direction> &listDir) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	player->stopEventWalk();

	player->sendCancelTarget();
	player->setFollowCreature(nullptr);

	player->resetIdleTime();
	player->setNextWalkTask(nullptr);

	player->startAutoWalk(listDir, true);
}

void Game::playerStopAutoWalk(uint32_t playerId) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	player->stopWalk();
}

void Game::playerUseItemEx(uint32_t playerId, const Position &fromPos, uint8_t fromStackPos, uint16_t fromItemId, const Position &toPos, uint8_t toStackPos, uint16_t toItemId) {
	metrics::method_latency measure(__METHOD_NAME__);
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	bool isHotkey = (fromPos.x == 0xFFFF && fromPos.y == 0 && fromPos.z == 0);
	if (isHotkey && !g_configManager().getBoolean(AIMBOT_HOTKEY_ENABLED, __FUNCTION__)) {
		return;
	}

	std::shared_ptr<Thing> thing = internalGetThing(player, fromPos, fromStackPos, fromItemId, STACKPOS_FIND_THING);
	if (!thing) {
		player->sendCancelMessage(RETURNVALUE_NOTPOSSIBLE);
		return;
	}

	std::shared_ptr<Item> item = thing->getItem();
	if (!item || !item->isMultiUse() || item->getID() != fromItemId) {
		player->sendCancelMessage(RETURNVALUE_CANNOTUSETHISOBJECT);
		return;
	}

	if (g_configManager().getBoolean(ONLY_INVITED_CAN_MOVE_HOUSE_ITEMS, __FUNCTION__) && !InternalGame::playerCanUseItemWithOnHouseTile(player, item, toPos, toStackPos, toItemId)) {
		player->sendCancelMessage(RETURNVALUE_CANNOTUSETHISOBJECT);
		return;
	}

	Position walkToPos = fromPos;
	ReturnValue ret = g_actions().canUse(player, fromPos);
	if (ret == RETURNVALUE_NOERROR) {
		ret = g_actions().canUse(player, toPos, item);
		if (ret == RETURNVALUE_TOOFARAWAY) {
			walkToPos = toPos;
		}
	}

	const ItemType &it = Item::items[item->getID()];
	if (it.isRune() || it.type == ITEM_TYPE_POTION) {
		if (player->walkExhausted()) {
			player->sendCancelMessage(RETURNVALUE_YOUAREEXHAUSTED);
			return;
		}
	}

	if (ret != RETURNVALUE_NOERROR) {
		if (ret == RETURNVALUE_TOOFARAWAY) {
			Position itemPos = fromPos;
			uint8_t itemStackPos = fromStackPos;

			if (fromPos.x != 0xFFFF && toPos.x != 0xFFFF && Position::areInRange<1, 1, 0>(fromPos, player->getPosition()) && !Position::areInRange<1, 1, 0>(fromPos, toPos)) {
				std::shared_ptr<Item> moveItem = nullptr;

				ret = internalMoveItem(item->getParent(), player, INDEX_WHEREEVER, item, item->getItemCount(), &moveItem);
				if (ret != RETURNVALUE_NOERROR) {
					player->sendCancelMessage(ret);
					return;
				}

				// changing the position since its now in the inventory of the player
				internalGetPosition(moveItem, itemPos, itemStackPos);
			}

			stdext::arraylist<Direction> listDir(128);
			if (player->getPathTo(walkToPos, listDir, 0, 1, true, true)) {
				g_dispatcher().addEvent(std::bind(&Game::playerAutoWalk, this, player->getID(), listDir.data()), "Game::playerAutoWalk");

				std::shared_ptr<Task> task = createPlayerTask(400, std::bind(&Game::playerUseItemEx, this, playerId, itemPos, itemStackPos, fromItemId, toPos, toStackPos, toItemId), "Game::playerUseItemEx");
				if (it.isRune() || it.type == ITEM_TYPE_POTION) {
					player->setNextPotionActionTask(task);
				} else {
					player->setNextWalkActionTask(task);
				}
			} else {
				player->sendCancelMessage(RETURNVALUE_THEREISNOWAY);
			}
			return;
		}

		player->sendCancelMessage(ret);
		return;
	}

	bool canDoAction = player->canDoAction();
	if (it.isRune() || it.type == ITEM_TYPE_POTION) {
		canDoAction = player->canDoPotionAction();
	}

	if (!canDoAction) {
		uint32_t delay = player->getNextActionTime();
		if (it.isRune() || it.type == ITEM_TYPE_POTION) {
			delay = player->getNextPotionActionTime();
		}
		std::shared_ptr<Task> task = createPlayerTask(delay, std::bind(&Game::playerUseItemEx, this, playerId, fromPos, fromStackPos, fromItemId, toPos, toStackPos, toItemId), "Game::playerUseItemEx");
		if (it.isRune() || it.type == ITEM_TYPE_POTION) {
			player->setNextPotionActionTask(task);
		} else {
			player->setNextActionTask(task);
		}
		return;
	}

	player->resetIdleTime();
	if (it.isRune() || it.type == ITEM_TYPE_POTION) {
		player->setNextPotionActionTask(nullptr);
	} else {
		player->setNextActionTask(nullptr);
	}

	// Refresh depot search window if necessary
	bool mustReloadDepotSearch = false;
	if (player->isDepotSearchOpenOnItem(fromItemId)) {
		if (item->isInsideDepot(true)) {
			mustReloadDepotSearch = true;
		} else {
			if (auto targetThing = internalGetThing(player, toPos, toStackPos, toItemId, STACKPOS_FIND_THING);
				targetThing && targetThing->getItem() && targetThing->getItem()->isInsideDepot(true)) {
				mustReloadDepotSearch = true;
			}
		}
	}

	g_actions().useItemEx(player, fromPos, toPos, toStackPos, item, isHotkey);

	if (mustReloadDepotSearch) {
		player->requestDepotSearchItem(fromItemId, fromStackPos);
	}
}

void Game::playerUseItem(uint32_t playerId, const Position &pos, uint8_t stackPos, uint8_t index, uint16_t itemId) {
	metrics::method_latency measure(__METHOD_NAME__);
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	bool isHotkey = (pos.x == 0xFFFF && pos.y == 0 && pos.z == 0);
	if (isHotkey && !g_configManager().getBoolean(AIMBOT_HOTKEY_ENABLED, __FUNCTION__)) {
		return;
	}

	std::shared_ptr<Thing> thing = internalGetThing(player, pos, stackPos, itemId, STACKPOS_FIND_THING);
	if (!thing) {
		player->sendCancelMessage(RETURNVALUE_NOTPOSSIBLE);
		return;
	}

	std::shared_ptr<Item> item = thing->getItem();
	if (!item || item->isMultiUse() || item->getID() != itemId) {
		player->sendCancelMessage(RETURNVALUE_CANNOTUSETHISOBJECT);
		return;
	}

	if (g_configManager().getBoolean(ONLY_INVITED_CAN_MOVE_HOUSE_ITEMS, __FUNCTION__) && !InternalGame::playerCanUseItemOnHouseTile(player, item)) {
		player->sendCancelMessage(RETURNVALUE_CANNOTUSETHISOBJECT);
		return;
	}

	const ItemType &it = Item::items[item->getID()];
	if (it.isRune() || it.type == ITEM_TYPE_POTION) {
		if (player->walkExhausted()) {
			player->sendCancelMessage(RETURNVALUE_YOUAREEXHAUSTED);
			return;
		}
	}

	ReturnValue ret = g_actions().canUse(player, pos);
	if (ret != RETURNVALUE_NOERROR) {
		if (ret == RETURNVALUE_TOOFARAWAY) {
			stdext::arraylist<Direction> listDir(128);
			if (player->getPathTo(pos, listDir, 0, 1, true, true)) {
				g_dispatcher().addEvent(std::bind(&Game::playerAutoWalk, this, player->getID(), listDir.data()), "Game::playerAutoWalk");

				std::shared_ptr<Task> task = createPlayerTask(400, std::bind(&Game::playerUseItem, this, playerId, pos, stackPos, index, itemId), "Game::playerUseItem");
				if (it.isRune() || it.type == ITEM_TYPE_POTION) {
					player->setNextPotionActionTask(task);
				} else {
					player->setNextWalkActionTask(task);
				}
				return;
			}

			ret = RETURNVALUE_THEREISNOWAY;
		}

		player->sendCancelMessage(ret);
		return;
	}

	bool canDoAction = player->canDoAction();
	if (it.isRune() || it.type == ITEM_TYPE_POTION) {
		canDoAction = player->canDoPotionAction();
	}

	if (!canDoAction) {
		uint32_t delay = player->getNextActionTime();
		if (it.isRune() || it.type == ITEM_TYPE_POTION) {
			delay = player->getNextPotionActionTime();
		}
		std::shared_ptr<Task> task = createPlayerTask(delay, std::bind(&Game::playerUseItem, this, playerId, pos, stackPos, index, itemId), "Game::playerUseItem");
		if (it.isRune() || it.type == ITEM_TYPE_POTION) {
			player->setNextPotionActionTask(task);
		} else {
			player->setNextActionTask(task);
		}
		return;
	}

	player->resetIdleTime();
	player->setNextActionTask(nullptr);

	// Refresh depot search window if necessary
	bool refreshDepotSearch = false;
	if (player->isDepotSearchOpenOnItem(itemId) && item->isInsideDepot(true)) {
		refreshDepotSearch = true;
	}

	g_actions().useItem(player, pos, index, item, isHotkey);

	if (refreshDepotSearch) {
		player->requestDepotSearchItem(itemId, stackPos);
	}
}

void Game::playerUseWithCreature(uint32_t playerId, const Position &fromPos, uint8_t fromStackPos, uint32_t creatureId, uint16_t itemId) {
	metrics::method_latency measure(__METHOD_NAME__);
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	std::shared_ptr<Creature> creature = getCreatureByID(creatureId);
	if (!creature) {
		return;
	}

	if (!Position::areInRange<7, 5, 0>(creature->getPosition(), player->getPosition())) {
		return;
	}

	bool isHotkey = (fromPos.x == 0xFFFF && fromPos.y == 0 && fromPos.z == 0);
	if (!g_configManager().getBoolean(AIMBOT_HOTKEY_ENABLED, __FUNCTION__)) {
		if (creature->getPlayer() || isHotkey) {
			player->sendCancelMessage(RETURNVALUE_DIRECTPLAYERSHOOT);
			return;
		}
	}

	std::shared_ptr<Thing> thing = internalGetThing(player, fromPos, fromStackPos, itemId, STACKPOS_FIND_THING);
	if (!thing) {
		player->sendCancelMessage(RETURNVALUE_NOTPOSSIBLE);
		return;
	}

	std::shared_ptr<Item> item = thing->getItem();
	if (!item || !item->isMultiUse() || item->getID() != itemId) {
		player->sendCancelMessage(RETURNVALUE_CANNOTUSETHISOBJECT);
		return;
	}

	if (g_configManager().getBoolean(ONLY_INVITED_CAN_MOVE_HOUSE_ITEMS, __FUNCTION__)) {
		if (std::shared_ptr<HouseTile> houseTile = std::dynamic_pointer_cast<HouseTile>(item->getTile())) {
			const auto &house = houseTile->getHouse();
			if (house && item->getRealParent() && item->getRealParent() != player && (!house->isInvited(player) || house->getHouseAccessLevel(player) == HOUSE_GUEST)) {
				player->sendCancelMessage(RETURNVALUE_CANNOTUSETHISOBJECT);
				return;
			}
		}
	}

	const ItemType &it = Item::items[item->getID()];
	if (it.isRune() || it.type == ITEM_TYPE_POTION) {
		if (player->walkExhausted()) {
			player->sendCancelMessage(RETURNVALUE_YOUAREEXHAUSTED);
			return;
		}
	}
	Position toPos = creature->getPosition();
	Position walkToPos = fromPos;
	ReturnValue ret = g_actions().canUse(player, fromPos);
	if (ret == RETURNVALUE_NOERROR) {
		ret = g_actions().canUse(player, toPos, item);
		if (ret == RETURNVALUE_TOOFARAWAY) {
			walkToPos = toPos;
		}
	}

	if (ret != RETURNVALUE_NOERROR) {
		if (ret == RETURNVALUE_TOOFARAWAY) {
			Position itemPos = fromPos;
			uint8_t itemStackPos = fromStackPos;

			if (fromPos.x != 0xFFFF && Position::areInRange<1, 1, 0>(fromPos, player->getPosition()) && !Position::areInRange<1, 1, 0>(fromPos, toPos)) {
				std::shared_ptr<Item> moveItem = nullptr;
				ret = internalMoveItem(item->getParent(), player, INDEX_WHEREEVER, item, item->getItemCount(), &moveItem);
				if (ret != RETURNVALUE_NOERROR) {
					player->sendCancelMessage(ret);
					return;
				}

				// changing the position since its now in the inventory of the player
				internalGetPosition(moveItem, itemPos, itemStackPos);
			}

			stdext::arraylist<Direction> listDir(128);
			if (player->getPathTo(walkToPos, listDir, 0, 1, true, true)) {
				g_dispatcher().addEvent(std::bind(&Game::playerAutoWalk, this, player->getID(), listDir.data()), "Game::playerAutoWalk");

				std::shared_ptr<Task> task = createPlayerTask(400, std::bind(&Game::playerUseWithCreature, this, playerId, itemPos, itemStackPos, creatureId, itemId), "Game::playerUseWithCreature");
				if (it.isRune() || it.type == ITEM_TYPE_POTION) {
					player->setNextPotionActionTask(task);
				} else {
					player->setNextWalkActionTask(task);
				}
			} else {
				player->sendCancelMessage(RETURNVALUE_THEREISNOWAY);
			}
			return;
		}

		player->sendCancelMessage(ret);
		return;
	}

	bool canDoAction = player->canDoAction();
	if (it.isRune() || it.type == ITEM_TYPE_POTION) {
		canDoAction = player->canDoPotionAction();
	}

	if (!canDoAction) {
		uint32_t delay = player->getNextActionTime();
		if (it.isRune() || it.type == ITEM_TYPE_POTION) {
			delay = player->getNextPotionActionTime();
		}
		std::shared_ptr<Task> task = createPlayerTask(delay, std::bind(&Game::playerUseWithCreature, this, playerId, fromPos, fromStackPos, creatureId, itemId), "Game::playerUseWithCreature");

		if (it.isRune() || it.type == ITEM_TYPE_POTION) {
			player->setNextPotionActionTask(task);
		} else {
			player->setNextActionTask(task);
		}
		return;
	}

	player->resetIdleTime();
	if (it.isRune() || it.type == ITEM_TYPE_POTION) {
		player->setNextPotionActionTask(nullptr);
	} else {
		player->setNextActionTask(nullptr);
	}

	g_actions().useItemEx(player, fromPos, creature->getPosition(), creature->getParent()->getThingIndex(creature), item, isHotkey, creature);
}

void Game::playerCloseContainer(uint32_t playerId, uint8_t cid) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	player->closeContainer(cid);
	player->sendCloseContainer(cid);
}

void Game::playerMoveUpContainer(uint32_t playerId, uint8_t cid) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	std::shared_ptr<Container> container = player->getContainerByID(cid);
	if (!container) {
		return;
	}

	std::shared_ptr<Container> parentContainer = std::dynamic_pointer_cast<Container>(container->getRealParent());
	if (!parentContainer) {
		std::shared_ptr<Tile> tile = container->getTile();
		if (!tile) {
			return;
		}

		if (!g_events().eventPlayerOnBrowseField(player, tile->getPosition())) {
			return;
		}

		if (!g_callbacks().checkCallback(EventCallback_t::playerOnBrowseField, &EventCallback::playerOnBrowseField, player, tile->getPosition())) {
			return;
		}

		auto it = browseFields.find(tile);
		if (it == browseFields.end() || it->second.expired()) {
			parentContainer = Container::create(tile);
			browseFields[tile] = parentContainer;
		} else {
			parentContainer = it->second.lock();
		}
	}

	if (parentContainer->hasPagination() && parentContainer->hasParent()) {
		uint16_t indexContainer = std::floor(parentContainer->getThingIndex(container) / parentContainer->capacity()) * parentContainer->capacity();
		player->addContainer(cid, parentContainer);

		player->setContainerIndex(cid, indexContainer);
		player->sendContainer(cid, parentContainer, parentContainer->hasParent(), indexContainer);
	} else {
		player->addContainer(cid, parentContainer);
		player->sendContainer(cid, parentContainer, parentContainer->hasParent(), player->getContainerIndex(cid));
	}
}

void Game::playerUpdateContainer(uint32_t playerId, uint8_t cid) {
	std::shared_ptr<Player> player = getPlayerByGUID(playerId);
	if (!player) {
		return;
	}

	std::shared_ptr<Container> container = player->getContainerByID(cid);
	if (!container) {
		return;
	}

	player->sendContainer(cid, container, container->hasParent(), player->getContainerIndex(cid));
}

void Game::playerRotateItem(uint32_t playerId, const Position &pos, uint8_t stackPos, const uint16_t itemId) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	std::shared_ptr<Thing> thing = internalGetThing(player, pos, stackPos, itemId, STACKPOS_TOPDOWN_ITEM);
	if (!thing) {
		return;
	}

	std::shared_ptr<Item> item = thing->getItem();
	if (!item || item->getID() != itemId || !item->isRotatable() || item->hasAttribute(ItemAttribute_t::UNIQUEID)) {
		player->sendCancelMessage(RETURNVALUE_NOTPOSSIBLE);
		return;
	}

	if (pos.x != 0xFFFF && !Position::areInRange<1, 1, 0>(pos, player->getPosition())) {
		stdext::arraylist<Direction> listDir(128);
		if (player->getPathTo(pos, listDir, 0, 1, true, true)) {
			g_dispatcher().addEvent(std::bind(&Game::playerAutoWalk, this, player->getID(), listDir.data()), "Game::playerAutoWalk");

			std::shared_ptr<Task> task = createPlayerTask(400, std::bind(&Game::playerRotateItem, this, playerId, pos, stackPos, itemId), "Game::playerRotateItem");
			player->setNextWalkActionTask(task);
		} else {
			player->sendCancelMessage(RETURNVALUE_THEREISNOWAY);
		}
		return;
	}

	if (!g_callbacks().checkCallback(EventCallback_t::playerOnRotateItem, &EventCallback::playerOnRotateItem, player, item, pos)) {
		return;
	}

	uint16_t newId = Item::items[item->getID()].rotateTo;
	if (newId != 0) {
		transformItem(item, newId);
	}
}

void Game::playerConfigureShowOffSocket(uint32_t playerId, const Position &pos, uint8_t stackPos, const uint16_t itemId) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player || pos.x == 0xFFFF) {
		return;
	}

	std::shared_ptr<Thing> thing = internalGetThing(player, pos, stackPos, itemId, STACKPOS_TOPDOWN_ITEM);
	if (!thing) {
		return;
	}

	std::shared_ptr<Item> item = thing->getItem();
	if (!item || item->getID() != itemId || !item->isPodium() || item->hasAttribute(ItemAttribute_t::UNIQUEID)) {
		player->sendCancelMessage(RETURNVALUE_NOTPOSSIBLE);
		return;
	}

	bool isPodiumOfRenown = itemId == ITEM_PODIUM_OF_RENOWN1 || itemId == ITEM_PODIUM_OF_RENOWN2;
	if (!Position::areInRange<1, 1, 0>(pos, player->getPosition())) {
		stdext::arraylist<Direction> listDir(128);
		if (player->getPathTo(pos, listDir, 0, 1, true, false)) {
			g_dispatcher().addEvent(std::bind(&Game::playerAutoWalk, this, player->getID(), listDir.data()), "Game::playerAutoWalk");
			std::shared_ptr<Task> task;
			if (isPodiumOfRenown) {
				task = createPlayerTask(400, std::bind_front(&Player::sendPodiumWindow, player, item, pos, itemId, stackPos), "Game::playerConfigureShowOffSocket");
			} else {
				task = createPlayerTask(400, std::bind_front(&Player::sendMonsterPodiumWindow, player, item, pos, itemId, stackPos), "Game::playerConfigureShowOffSocket");
			}
			player->setNextWalkActionTask(task);
		} else {
			player->sendCancelMessage(RETURNVALUE_THEREISNOWAY);
		}
		return;
	}

	if (isPodiumOfRenown) {
		player->sendPodiumWindow(item, pos, itemId, stackPos);
	} else {
		player->sendMonsterPodiumWindow(item, pos, itemId, stackPos);
	}
}

void Game::playerSetShowOffSocket(uint32_t playerId, Outfit_t &outfit, const Position &pos, uint8_t stackPos, const uint16_t itemId, uint8_t podiumVisible, uint8_t direction) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player || pos.x == 0xFFFF) {
		return;
	}

	std::shared_ptr<Thing> thing = internalGetThing(player, pos, stackPos, itemId, STACKPOS_TOPDOWN_ITEM);
	if (!thing) {
		return;
	}

	std::shared_ptr<Item> item = thing->getItem();
	if (!item || item->getID() != itemId || !item->isPodium() || item->hasAttribute(ItemAttribute_t::UNIQUEID)) {
		player->sendCancelMessage(RETURNVALUE_NOTPOSSIBLE);
		return;
	}

	const auto tile = item->getParent() ? item->getParent()->getTile() : nullptr;
	if (!tile) {
		player->sendCancelMessage(RETURNVALUE_NOTPOSSIBLE);
		return;
	}

	if (!Position::areInRange<1, 1, 0>(pos, player->getPosition())) {
		stdext::arraylist<Direction> listDir(128);
		if (player->getPathTo(pos, listDir, 0, 1, true, false)) {
			g_dispatcher().addEvent(std::bind(&Game::playerAutoWalk, this, player->getID(), listDir.data()), "Game::playerAutoWalk");
			std::shared_ptr<Task> task = createPlayerTask(400, std::bind(&Game::playerBrowseField, this, playerId, pos), "Game::playerBrowseField");
			player->setNextWalkActionTask(task);
		} else {
			player->sendCancelMessage(RETURNVALUE_THEREISNOWAY);
		}
		return;
	}

	if (g_configManager().getBoolean(ONLY_INVITED_CAN_MOVE_HOUSE_ITEMS, __FUNCTION__) && !InternalGame::playerCanUseItemOnHouseTile(player, item)) {
		player->sendCancelMessage(RETURNVALUE_NOTPOSSIBLE);
		return;
	}

	if (outfit.lookType != 0) {
		item->setCustomAttribute("PastLookType", static_cast<int64_t>(outfit.lookType));
	}

	if (outfit.lookMount != 0) {
		item->setCustomAttribute("PastLookMount", static_cast<int64_t>(outfit.lookMount));
	}

	if (!player->canWear(outfit.lookType, outfit.lookAddons)) {
		outfit.lookType = 0;
		outfit.lookAddons = 0;
	}

	const auto mount = mounts.getMountByClientID(outfit.lookMount);
	if (!mount || !player->hasMount(mount)) {
		outfit.lookMount = 0;
	}

	if (outfit.lookType != 0) {
		item->setCustomAttribute("LookType", static_cast<int64_t>(outfit.lookType));
		item->setCustomAttribute("LookHead", static_cast<int64_t>(outfit.lookHead));
		item->setCustomAttribute("LookBody", static_cast<int64_t>(outfit.lookBody));
		item->setCustomAttribute("LookLegs", static_cast<int64_t>(outfit.lookLegs));
		item->setCustomAttribute("LookFeet", static_cast<int64_t>(outfit.lookFeet));
		item->setCustomAttribute("LookAddons", static_cast<int64_t>(outfit.lookAddons));
	} else if (auto pastLookType = item->getCustomAttribute("PastLookType");
			   pastLookType && pastLookType->getInteger() > 0) {
		item->removeCustomAttribute("LookType");
		item->removeCustomAttribute("PastLookType");
	}

	if (outfit.lookMount != 0) {
		item->setCustomAttribute("LookMount", static_cast<int64_t>(outfit.lookMount));
		item->setCustomAttribute("LookMountHead", static_cast<int64_t>(outfit.lookMountHead));
		item->setCustomAttribute("LookMountBody", static_cast<int64_t>(outfit.lookMountBody));
		item->setCustomAttribute("LookMountLegs", static_cast<int64_t>(outfit.lookMountLegs));
		item->setCustomAttribute("LookMountFeet", static_cast<int64_t>(outfit.lookMountFeet));
	} else if (auto pastLookMount = item->getCustomAttribute("PastLookMount");
			   pastLookMount && pastLookMount->getInteger() > 0) {
		item->removeCustomAttribute("LookMount");
		item->removeCustomAttribute("PastLookMount");
	}

	item->setCustomAttribute("PodiumVisible", static_cast<int64_t>(podiumVisible));
	item->setCustomAttribute("LookDirection", static_cast<int64_t>(direction));

	// Change Podium name
	if (outfit.lookType != 0 || outfit.lookMount != 0) {
		std::ostringstream name;
		name << item->getName() << " displaying the ";
		bool outfited = false;
		if (outfit.lookType != 0) {
			const auto &outfitInfo = Outfits::getInstance().getOutfitByLookType(player->getSex(), outfit.lookType);
			if (!outfitInfo) {
				return;
			}

			name << outfitInfo->name << " outfit";
			outfited = true;
		}

		if (outfit.lookMount != 0) {
			if (outfited) {
				name << " on the ";
			}
			name << mount->name << " mount";
		}
		item->setAttribute(ItemAttribute_t::NAME, name.str());
	} else {
		item->removeAttribute(ItemAttribute_t::NAME);
	}

	// Send to client
	for (const auto &spectator : Spectators().find<Player>(pos, true)) {
		spectator->getPlayer()->sendUpdateTileItem(tile, pos, item);
	}
}

void Game::playerWrapableItem(uint32_t playerId, const Position &pos, uint8_t stackPos, const uint16_t itemId) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	const auto &house = map.houses.getHouseByPlayerId(player->getGUID());
	if (!house) {
		player->sendCancelMessage("You don't own a house, you need own a house to use this.");
		return;
	}

	std::shared_ptr<Thing> thing = internalGetThing(player, pos, stackPos, itemId, STACKPOS_FIND_THING);
	if (!thing) {
		return;
	}

	std::shared_ptr<Item> item = thing->getItem();
	std::shared_ptr<Tile> tile = map.getTile(item->getPosition());
	std::shared_ptr<HouseTile> houseTile = std::dynamic_pointer_cast<HouseTile>(tile);
	if (!tile->hasFlag(TILESTATE_PROTECTIONZONE) || !houseTile) {
		player->sendCancelMessage("You may construct this only inside a house.");
		return;
	}
	if (houseTile->getHouse() != house) {
		player->sendCancelMessage("Only owners can wrap/unwrap inside a house.");
		return;
	}

	if (!item || item->getID() != itemId || item->hasAttribute(ItemAttribute_t::UNIQUEID) || (!item->isWrapable() && item->getID() != ITEM_DECORATION_KIT)) {
		player->sendCancelMessage(RETURNVALUE_NOTPOSSIBLE);
		return;
	}

	if (pos.x != 0xFFFF && !Position::areInRange<1, 1, 0>(pos, player->getPosition())) {
		stdext::arraylist<Direction> listDir(128);
		if (player->getPathTo(pos, listDir, 0, 1, true, true)) {
			g_dispatcher().addEvent(std::bind(&Game::playerAutoWalk, this, player->getID(), listDir.data()), "Game::playerAutoWalk");

			std::shared_ptr<Task> task = createPlayerTask(400, std::bind(&Game::playerWrapableItem, this, playerId, pos, stackPos, itemId), "Game::playerWrapableItem");
			player->setNextWalkActionTask(task);
		} else {
			player->sendCancelMessage(RETURNVALUE_THEREISNOWAY);
		}
		return;
	}

	std::shared_ptr<Container> container = item->getContainer();
	if (container && container->getItemHoldingCount() > 0) {
		player->sendCancelMessage(RETURNVALUE_NOTPOSSIBLE);
		return;
	}

	auto topItem = tile->getTopTopItem();
	bool unwrappable = item->getHoldingPlayer() && item->getID() == ITEM_DECORATION_KIT;
	bool blockedUnwrap = topItem && topItem->canReceiveAutoCarpet() && !item->hasProperty(CONST_PROP_IMMOVABLEBLOCKSOLID);

	if (unwrappable || blockedUnwrap) {
		player->sendCancelMessage("You can only wrap/unwrap on the floor.");
		return;
	}

	std::string itemName = item->getName();
	auto unWrapAttribute = item->getCustomAttribute("unWrapId");
	uint16_t unWrapId = 0;
	if (unWrapAttribute != nullptr) {
		unWrapId = static_cast<uint16_t>(unWrapAttribute->getInteger());
	}

	// Prevent to wrap a filled bath tube
	if (item->getID() == ITEM_FILLED_BATH_TUBE) {
		player->sendCancelMessage(RETURNVALUE_NOTPOSSIBLE);
		return;
	}

	if (item->isWrapable() && item->getID() != ITEM_DECORATION_KIT) {
		wrapItem(item, houseTile->getHouse());
	} else if (item->getID() == ITEM_DECORATION_KIT && unWrapId != 0) {
		unwrapItem(item, unWrapId, houseTile->getHouse(), player);
	}
	addMagicEffect(pos, CONST_ME_POFF);
}

std::shared_ptr<Item> Game::wrapItem(std::shared_ptr<Item> item, std::shared_ptr<House> house) {
	uint16_t hiddenCharges = 0;
	uint16_t amount = item->getItemCount();
	if (isCaskItem(item->getID())) {
		hiddenCharges = item->getSubType();
	}
	if (house != nullptr && Item::items.getItemType(item->getID()).isBed()) {
		item->getBed()->wakeUp(nullptr);
		house->removeBed(item->getBed());
	}
	uint16_t oldItemID = item->getID();
	auto itemName = item->getName();
	std::shared_ptr<Item> newItem = transformItem(item, ITEM_DECORATION_KIT);
	newItem->setCustomAttribute("unWrapId", static_cast<int64_t>(oldItemID));
	newItem->setAttribute(ItemAttribute_t::DESCRIPTION, "Unwrap it in your own house to create a <" + itemName + ">.");
	if (hiddenCharges > 0) {
		newItem->setAttribute(DATE, hiddenCharges);
	}
	if (amount > 0) {
		newItem->setAttribute(AMOUNT, amount);
	}
	newItem->startDecaying();
	return newItem;
}

void Game::unwrapItem(std::shared_ptr<Item> item, uint16_t unWrapId, std::shared_ptr<House> house, std::shared_ptr<Player> player) {
	auto hiddenCharges = item->getAttribute<uint16_t>(DATE);
	const ItemType &newiType = Item::items.getItemType(unWrapId);
	if (player != nullptr && house != nullptr && newiType.isBed() && house->getMaxBeds() > -1 && house->getBedCount() >= house->getMaxBeds()) {
		player->sendCancelMessage("You reached the maximum beds in this house");
		return;
	}
	auto amount = item->getAttribute<uint16_t>(AMOUNT);
	if (!amount) {
		amount = 1;
	}
	std::shared_ptr<Item> newItem = transformItem(item, unWrapId, amount);
	if (house && newiType.isBed()) {
		house->addBed(newItem->getBed());
	}
	if (newItem) {
		if (hiddenCharges > 0 && isCaskItem(unWrapId)) {
			newItem->setSubType(hiddenCharges);
		}
		newItem->removeCustomAttribute("unWrapId");
		newItem->removeAttribute(DESCRIPTION);
		newItem->startDecaying();
	}
}

void Game::playerWriteItem(uint32_t playerId, uint32_t windowTextId, const std::string &text) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	uint16_t maxTextLength = 0;
	uint32_t internalWindowTextId = 0;

	std::shared_ptr<Item> writeItem = player->getWriteItem(internalWindowTextId, maxTextLength);
	if (text.length() > maxTextLength || windowTextId != internalWindowTextId) {
		return;
	}

	if (!writeItem || writeItem->isRemoved()) {
		player->sendCancelMessage(RETURNVALUE_NOTPOSSIBLE);
		return;
	}

	std::shared_ptr<Cylinder> topParent = writeItem->getTopParent();

	std::shared_ptr<Player> owner = std::dynamic_pointer_cast<Player>(topParent);
	if (owner && owner != player) {
		player->sendCancelMessage(RETURNVALUE_NOTPOSSIBLE);
		return;
	}

	if (!Position::areInRange<1, 1, 0>(writeItem->getPosition(), player->getPosition())) {
		player->sendCancelMessage(RETURNVALUE_NOTPOSSIBLE);
		return;
	}

	for (auto creatureEvent : player->getCreatureEvents(CREATURE_EVENT_TEXTEDIT)) {
		if (!creatureEvent->executeTextEdit(player, writeItem, text)) {
			player->setWriteItem(nullptr);
			return;
		}
	}

	if (!text.empty()) {
		if (writeItem->getAttribute<std::string>(ItemAttribute_t::TEXT) != text) {
			writeItem->setAttribute(ItemAttribute_t::TEXT, text);
			writeItem->setAttribute(ItemAttribute_t::WRITER, player->getName());
			writeItem->setAttribute(ItemAttribute_t::DATE, getTimeNow());
		}
	} else {
		writeItem->removeAttribute(ItemAttribute_t::TEXT);
		writeItem->removeAttribute(ItemAttribute_t::WRITER);
		writeItem->removeAttribute(ItemAttribute_t::DATE);
	}

	uint16_t newId = Item::items[writeItem->getID()].writeOnceItemId;
	if (newId != 0) {
		transformItem(writeItem, newId);
	}

	player->setWriteItem(nullptr);
}

void Game::playerBrowseField(uint32_t playerId, const Position &pos) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	const Position &playerPos = player->getPosition();
	if (playerPos.z != pos.z) {
		player->sendCancelMessage(playerPos.z > pos.z ? RETURNVALUE_FIRSTGOUPSTAIRS : RETURNVALUE_FIRSTGODOWNSTAIRS);
		return;
	}

	if (!Position::areInRange<1, 1>(playerPos, pos)) {
		stdext::arraylist<Direction> listDir(128);
		if (player->getPathTo(pos, listDir, 0, 1, true, true)) {
			g_dispatcher().addEvent(std::bind(&Game::playerAutoWalk, this, player->getID(), listDir.data()), "Game::playerAutoWalk");
			std::shared_ptr<Task> task = createPlayerTask(400, std::bind(&Game::playerBrowseField, this, playerId, pos), "Game::playerBrowseField");
			player->setNextWalkActionTask(task);
		} else {
			player->sendCancelMessage(RETURNVALUE_THEREISNOWAY);
		}
		return;
	}

	std::shared_ptr<Tile> tile = map.getTile(pos);
	if (!tile) {
		return;
	}

	if (!g_events().eventPlayerOnBrowseField(player, pos)) {
		return;
	}

	if (!g_callbacks().checkCallback(EventCallback_t::playerOnBrowseField, &EventCallback::playerOnBrowseField, player, tile->getPosition())) {
		return;
	}

	std::shared_ptr<Container> container;

	auto it = browseFields.find(tile);
	if (it == browseFields.end() || it->second.expired()) {
		container = Container::create(tile);
		browseFields[tile] = container;
	} else {
		container = it->second.lock();
	}

	uint8_t dummyContainerId = 0xF - ((pos.x % 3) * 3 + (pos.y % 3));
	std::shared_ptr<Container> openContainer = player->getContainerByID(dummyContainerId);
	if (openContainer) {
		player->onCloseContainer(openContainer);
		player->closeContainer(dummyContainerId);
	} else {
		player->addContainer(dummyContainerId, container);
		player->sendContainer(dummyContainerId, container, false, 0);
	}
}

void Game::playerStowItem(uint32_t playerId, const Position &pos, uint16_t itemId, uint8_t stackpos, uint8_t count, bool allItems) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	if (!player->isPremium()) {
		player->sendCancelMessage(RETURNVALUE_YOUNEEDPREMIUMACCOUNT);
		return;
	}

	std::shared_ptr<Thing> thing = internalGetThing(player, pos, stackpos, itemId, STACKPOS_TOPDOWN_ITEM);
	if (!thing) {
		return;
	}

	std::shared_ptr<Item> item = thing->getItem();
	if (!item || item->getID() != itemId || item->getItemCount() < count || item->isStoreItem()) {
		player->sendCancelMessage(RETURNVALUE_NOTPOSSIBLE);
		return;
	}

	if (pos.x != 0xFFFF && !Position::areInRange<1, 1, 0>(pos, player->getPosition())) {
		player->sendCancelMessage(RETURNVALUE_NOTPOSSIBLE);
		return;
	}

	player->stowItem(item, count, allItems);

	// Refresh depot search window if necessary
	if (player->isDepotSearchOpenOnItem(itemId)) {
		// Tier for item stackable is 0
		player->requestDepotSearchItem(itemId, 0);
	}
}

void Game::playerStashWithdraw(uint32_t playerId, uint16_t itemId, uint32_t count, uint8_t) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	if (player->hasFlag(PlayerFlags_t::CannotPickupItem)) {
		return;
	}

	const ItemType &it = Item::items[itemId];
	if (it.id == 0 || count == 0) {
		return;
	}

	uint16_t freeSlots = player->getFreeBackpackSlots();
	auto stashContainer = player->getLootContainer(OBJECTCATEGORY_STASHRETRIEVE);
	if (stashContainer && !(player->quickLootFallbackToMainContainer)) {
		freeSlots = stashContainer->getFreeSlots();
	}

	if (freeSlots == 0) {
		player->sendCancelMessage(RETURNVALUE_NOTENOUGHROOM);
		return;
	}

	if (player->getFreeCapacity() < 100) {
		player->sendCancelMessage(RETURNVALUE_NOTENOUGHCAPACITY);
		return;
	}

	int32_t NDSlots = ((freeSlots) - (count < it.stackSize ? 1 : (count / it.stackSize)));
	uint32_t SlotsWith = count;
	uint32_t noSlotsWith = 0;

	if (NDSlots <= 0) {
		SlotsWith = (freeSlots * it.stackSize);
		noSlotsWith = (count - SlotsWith);
	}

	uint32_t capWith = count;
	uint32_t noCapWith = 0;
	if (player->getFreeCapacity() < (count * it.weight)) {
		capWith = (player->getFreeCapacity() / it.weight);
		noCapWith = (count - capWith);
	}

	std::stringstream ss;
	uint32_t WithdrawCount = (SlotsWith > capWith ? capWith : SlotsWith);
	uint32_t NoWithdrawCount = (noSlotsWith < noCapWith ? noCapWith : noSlotsWith);
	const char* NoWithdrawMsg = (noSlotsWith < noCapWith ? "capacity" : "slots");

	if (WithdrawCount != count) {
		ss << "Retrieved " << WithdrawCount << "x " << it.name << ".\n";
		ss << NoWithdrawCount << "x are impossible to retrieve due to insufficient inventory " << NoWithdrawMsg << ".";
	} else {
		ss << "Retrieved " << WithdrawCount << "x " << it.name << '.';
	}

	player->sendTextMessage(MESSAGE_STATUS, ss.str());

	if (player->withdrawItem(itemId, WithdrawCount)) {
		player->addItemFromStash(it.id, WithdrawCount);
	} else {
		player->sendCancelMessage(RETURNVALUE_NOTPOSSIBLE);
	}

	// Refresh depot search window if necessary
	if (player->isDepotSearchOpenOnItem(itemId)) {
		player->requestDepotSearchItem(itemId, 0);
	}

	player->sendOpenStash(true);
}

void Game::playerSeekInContainer(uint32_t playerId, uint8_t containerId, uint16_t index, uint8_t containerCategory) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	std::shared_ptr<Container> container = player->getContainerByID(containerId);
	if (!container || !container->hasPagination()) {
		return;
	}

	if (container->isStoreInbox()) {
		auto enumName = magic_enum::enum_name(static_cast<ContainerCategory_t>(containerCategory)).data();
		container->setAttribute(ItemAttribute_t::STORE_INBOX_CATEGORY, enumName);
		g_logger().debug("Setting new container with store inbox category name {}", enumName);
	}

	if ((index % container->capacity()) != 0 || index >= container->size()) {
		return;
	}

	player->setContainerIndex(containerId, index);
	player->sendContainer(containerId, container, container->hasParent(), index);
}

void Game::playerUpdateHouseWindow(uint32_t playerId, uint8_t listId, uint32_t windowTextId, const std::string &text) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	uint32_t internalWindowTextId;
	uint32_t internalListId;

	const auto &house = player->getEditHouse(internalWindowTextId, internalListId);
	if (house && house->canEditAccessList(internalListId, player) && internalWindowTextId == windowTextId && listId == 0) {
		house->setAccessList(internalListId, text);
	}

	player->setEditHouse(nullptr);
}

void Game::playerRequestTrade(uint32_t playerId, const Position &pos, uint8_t stackPos, uint32_t tradePlayerId, uint16_t itemId) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	std::shared_ptr<Player> tradePartner = getPlayerByID(tradePlayerId);
	if (!tradePartner || tradePartner == player) {
		player->sendTextMessage(MESSAGE_FAILURE, "Sorry, not possible.");
		return;
	}

	if (!Position::areInRange<2, 2, 0>(tradePartner->getPosition(), player->getPosition())) {
		std::ostringstream ss;
		ss << tradePartner->getName() << " tells you to move closer.";
		player->sendTextMessage(MESSAGE_TRADE, ss.str());
		return;
	}

	if (!canThrowObjectTo(tradePartner->getPosition(), player->getPosition())) {
		player->sendCancelMessage(RETURNVALUE_CREATUREISNOTREACHABLE);
		return;
	}

	std::shared_ptr<Thing> tradeThing = internalGetThing(player, pos, stackPos, itemId, STACKPOS_TOPDOWN_ITEM);
	if (!tradeThing) {
		player->sendCancelMessage(RETURNVALUE_NOTPOSSIBLE);
		return;
	}

	std::shared_ptr<Item> tradeItem = tradeThing->getItem();
	if (tradeItem->getID() != itemId || !tradeItem->isPickupable() || tradeItem->hasAttribute(ItemAttribute_t::UNIQUEID)) {
		player->sendCancelMessage(RETURNVALUE_NOTPOSSIBLE);
		return;
	}

	if (g_configManager().getBoolean(ONLY_INVITED_CAN_MOVE_HOUSE_ITEMS, __FUNCTION__)) {
		if (std::shared_ptr<HouseTile> houseTile = std::dynamic_pointer_cast<HouseTile>(tradeItem->getTile())) {
			const auto &house = houseTile->getHouse();
			if (house && tradeItem->getRealParent() != player && (!house->isInvited(player) || house->getHouseAccessLevel(player) == HOUSE_GUEST)) {
				player->sendCancelMessage(RETURNVALUE_NOTMOVEABLE);
				return;
			}
		}
	}

	const Position &playerPosition = player->getPosition();
	const Position &tradeItemPosition = tradeItem->getPosition();
	if (playerPosition.z != tradeItemPosition.z) {
		player->sendCancelMessage(playerPosition.z > tradeItemPosition.z ? RETURNVALUE_FIRSTGOUPSTAIRS : RETURNVALUE_FIRSTGODOWNSTAIRS);
		return;
	}

	if (!Position::areInRange<1, 1>(tradeItemPosition, playerPosition)) {
		stdext::arraylist<Direction> listDir(128);
		if (player->getPathTo(pos, listDir, 0, 1, true, true)) {
			g_dispatcher().addEvent(std::bind(&Game::playerAutoWalk, this, player->getID(), listDir.data()), "Game::playerAutoWalk");

			std::shared_ptr<Task> task = createPlayerTask(400, std::bind(&Game::playerRequestTrade, this, playerId, pos, stackPos, tradePlayerId, itemId), "Game::playerRequestTrade");
			player->setNextWalkActionTask(task);
		} else {
			player->sendCancelMessage(RETURNVALUE_THEREISNOWAY);
		}
		return;
	}

	std::shared_ptr<Container> tradeItemContainer = tradeItem->getContainer();
	if (tradeItemContainer) {
		for (const auto &it : tradeItems) {
			std::shared_ptr<Item> item = it.first;
			if (tradeItem == item) {
				player->sendTextMessage(MESSAGE_TRADE, "This item is already being traded.");
				return;
			}

			if (tradeItemContainer->isHoldingItem(item)) {
				player->sendTextMessage(MESSAGE_TRADE, "This item is already being traded.");
				return;
			}

			std::shared_ptr<Container> container = item->getContainer();
			if (container && container->isHoldingItem(tradeItem)) {
				player->sendTextMessage(MESSAGE_TRADE, "This item is already being traded.");
				return;
			}
		}
	} else {
		for (const auto &it : tradeItems) {
			std::shared_ptr<Item> item = it.first;
			if (tradeItem == item) {
				player->sendTextMessage(MESSAGE_TRADE, "This item is already being traded.");
				return;
			}

			std::shared_ptr<Container> container = item->getContainer();
			if (container && container->isHoldingItem(tradeItem)) {
				player->sendTextMessage(MESSAGE_TRADE, "This item is already being traded.");
				return;
			}
		}
	}

	auto tradeContainer = tradeItem->getContainer();
	if (tradeContainer && tradeContainer->getItemHoldingCount() + 1 > 100) {
		player->sendTextMessage(MESSAGE_TRADE, "You can not trade more than 100 items.");
		return;
	}

	if (tradeItem->isStoreItem()) {
		player->sendTextMessage(MESSAGE_TRADE, "This item cannot be trade.");
		return;
	}

	if (tradeItemContainer) {
		for (std::shared_ptr<Item> containerItem : tradeItemContainer->getItems(true)) {
			if (containerItem->isStoreItem()) {
				player->sendTextMessage(MESSAGE_TRADE, "This item cannot be trade.");
				return;
			}
		}
	}

	if (!g_events().eventPlayerOnTradeRequest(player, tradePartner, tradeItem)) {
		return;
	}

	if (!g_callbacks().checkCallback(EventCallback_t::playerOnTradeRequest, &EventCallback::playerOnTradeRequest, player, tradePartner, tradeItem)) {
		return;
	}

	internalStartTrade(player, tradePartner, tradeItem);
}

bool Game::internalStartTrade(std::shared_ptr<Player> player, std::shared_ptr<Player> tradePartner, std::shared_ptr<Item> tradeItem) {
	if (player->tradeState != TRADE_NONE && !(player->tradeState == TRADE_ACKNOWLEDGE && player->tradePartner == tradePartner)) {
		player->sendCancelMessage(RETURNVALUE_YOUAREALREADYTRADING);
		return false;
	} else if (tradePartner->tradeState != TRADE_NONE && tradePartner->tradePartner != player) {
		player->sendCancelMessage(RETURNVALUE_THISPLAYERISALREADYTRADING);
		return false;
	}

	player->tradePartner = tradePartner;
	player->tradeItem = tradeItem;
	player->tradeState = TRADE_INITIATED;
	tradeItems[tradeItem] = player->getID();

	player->sendTradeItemRequest(player->getName(), tradeItem, true);

	if (tradePartner->tradeState == TRADE_NONE) {
		std::ostringstream ss;
		ss << player->getName() << " wants to trade with you.";
		tradePartner->sendTextMessage(MESSAGE_TRANSACTION, ss.str());
		tradePartner->tradeState = TRADE_ACKNOWLEDGE;
		tradePartner->tradePartner = player;
	} else {
		std::shared_ptr<Item> counterOfferItem = tradePartner->tradeItem;
		player->sendTradeItemRequest(tradePartner->getName(), counterOfferItem, false);
		tradePartner->sendTradeItemRequest(player->getName(), tradeItem, false);
	}

	return true;
}

void Game::playerAcceptTrade(uint32_t playerId) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	if (!(player->getTradeState() == TRADE_ACKNOWLEDGE || player->getTradeState() == TRADE_INITIATED)) {
		return;
	}

	std::shared_ptr<Player> tradePartner = player->tradePartner;
	if (!tradePartner) {
		return;
	}

	if (!canThrowObjectTo(tradePartner->getPosition(), player->getPosition())) {
		player->sendCancelMessage(RETURNVALUE_CREATUREISNOTREACHABLE);
		return;
	}

	player->setTradeState(TRADE_ACCEPT);

	if (tradePartner->getTradeState() == TRADE_ACCEPT) {
		std::shared_ptr<Item> tradeItem1 = player->tradeItem;
		std::shared_ptr<Item> tradeItem2 = tradePartner->tradeItem;
		if (!g_events().eventPlayerOnTradeAccept(player, tradePartner, tradeItem1, tradeItem2)) {
			internalCloseTrade(player);
			return;
		}

		if (!g_callbacks().checkCallback(EventCallback_t::playerOnTradeAccept, &EventCallback::playerOnTradeAccept, player, tradePartner, tradeItem1, tradeItem2)) {
			internalCloseTrade(player);
			return;
		}

		player->setTradeState(TRADE_TRANSFER);
		tradePartner->setTradeState(TRADE_TRANSFER);

		auto it = tradeItems.find(tradeItem1);
		if (it != tradeItems.end()) {
			tradeItems.erase(it);
		}

		it = tradeItems.find(tradeItem2);
		if (it != tradeItems.end()) {
			tradeItems.erase(it);
		}

		bool isSuccess = false;

		ReturnValue ret1 = internalAddItem(tradePartner, tradeItem1, INDEX_WHEREEVER, 0, true);
		ReturnValue ret2 = internalAddItem(player, tradeItem2, INDEX_WHEREEVER, 0, true);
		if (ret1 == RETURNVALUE_NOERROR && ret2 == RETURNVALUE_NOERROR) {
			ret1 = internalRemoveItem(tradeItem1, tradeItem1->getItemCount(), true);
			ret2 = internalRemoveItem(tradeItem2, tradeItem2->getItemCount(), true);
			if (ret1 == RETURNVALUE_NOERROR && ret2 == RETURNVALUE_NOERROR) {
				std::shared_ptr<Cylinder> cylinder1 = tradeItem1->getParent();
				std::shared_ptr<Cylinder> cylinder2 = tradeItem2->getParent();

				uint32_t count1 = tradeItem1->getItemCount();
				uint32_t count2 = tradeItem2->getItemCount();

				ret1 = internalMoveItem(cylinder1, tradePartner, INDEX_WHEREEVER, tradeItem1, count1, nullptr, FLAG_IGNOREAUTOSTACK, nullptr, tradeItem2);
				if (ret1 == RETURNVALUE_NOERROR) {
					internalMoveItem(cylinder2, player, INDEX_WHEREEVER, tradeItem2, count2, nullptr, FLAG_IGNOREAUTOSTACK);

					tradeItem1->onTradeEvent(ON_TRADE_TRANSFER, tradePartner);
					tradeItem2->onTradeEvent(ON_TRADE_TRANSFER, player);

					isSuccess = true;
				}
			}
		}

		if (!isSuccess) {
			std::string errorDescription;

			if (tradePartner->tradeItem) {
				errorDescription = getTradeErrorDescription(ret1, tradeItem1);
				tradePartner->sendTextMessage(MESSAGE_TRANSACTION, errorDescription);
				tradePartner->tradeItem->onTradeEvent(ON_TRADE_CANCEL, tradePartner);
			}

			if (player->tradeItem) {
				errorDescription = getTradeErrorDescription(ret2, tradeItem2);
				player->sendTextMessage(MESSAGE_TRANSACTION, errorDescription);
				player->tradeItem->onTradeEvent(ON_TRADE_CANCEL, player);
			}
		}

		player->setTradeState(TRADE_NONE);
		player->tradeItem = nullptr;
		player->tradePartner = nullptr;
		player->sendTradeClose();

		tradePartner->setTradeState(TRADE_NONE);
		tradePartner->tradeItem = nullptr;
		tradePartner->tradePartner = nullptr;
		tradePartner->sendTradeClose();
	}
}

std::string Game::getTradeErrorDescription(ReturnValue ret, std::shared_ptr<Item> item) {
	if (item) {
		if (ret == RETURNVALUE_NOTENOUGHCAPACITY) {
			std::ostringstream ss;
			ss << "You do not have enough capacity to carry";

			if (item->isStackable() && item->getItemCount() > 1) {
				ss << " these objects.";
			} else {
				ss << " this object.";
			}

			ss << std::endl
			   << ' ' << item->getWeightDescription();
			return ss.str();
		} else if (ret == RETURNVALUE_NOTENOUGHROOM || ret == RETURNVALUE_CONTAINERNOTENOUGHROOM) {
			std::ostringstream ss;
			ss << "You do not have enough room to carry";

			if (item->isStackable() && item->getItemCount() > 1) {
				ss << " these objects.";
			} else {
				ss << " this object.";
			}

			return ss.str();
		}
	}
	return "Trade could not be completed.";
}

void Game::playerLookInTrade(uint32_t playerId, bool lookAtCounterOffer, uint8_t index) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	std::shared_ptr<Player> tradePartner = player->tradePartner;
	if (!tradePartner) {
		return;
	}

	std::shared_ptr<Item> tradeItem;
	if (lookAtCounterOffer) {
		tradeItem = tradePartner->getTradeItem();
	} else {
		tradeItem = player->getTradeItem();
	}

	if (!tradeItem) {
		return;
	}

	const Position &playerPosition = player->getPosition();
	const Position &tradeItemPosition = tradeItem->getPosition();

	int32_t lookDistance = std::max<int32_t>(
		Position::getDistanceX(playerPosition, tradeItemPosition),
		Position::getDistanceY(playerPosition, tradeItemPosition)
	);
	if (index == 0) {
		g_events().eventPlayerOnLookInTrade(player, tradePartner, tradeItem, lookDistance);
		g_callbacks().executeCallback(EventCallback_t::playerOnLookInTrade, &EventCallback::playerOnLookInTrade, player, tradePartner, tradeItem, lookDistance);
		return;
	}

	std::shared_ptr<Container> tradeContainer = tradeItem->getContainer();
	if (!tradeContainer) {
		return;
	}

	std::vector<std::shared_ptr<Container>> containers { tradeContainer };
	size_t i = 0;
	while (i < containers.size()) {
		std::shared_ptr<Container> container = containers[i++];
		for (std::shared_ptr<Item> item : container->getItemList()) {
			std::shared_ptr<Container> tmpContainer = item->getContainer();
			if (tmpContainer) {
				containers.push_back(tmpContainer);
			}

			if (--index == 0) {
				g_events().eventPlayerOnLookInTrade(player, tradePartner, item, lookDistance);
				g_callbacks().executeCallback(EventCallback_t::playerOnLookInTrade, &EventCallback::playerOnLookInTrade, player, tradePartner, item, lookDistance);
				return;
			}
		}
	}
}

void Game::playerCloseTrade(uint32_t playerId) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	internalCloseTrade(player);
}

void Game::internalCloseTrade(std::shared_ptr<Player> player) {
	std::shared_ptr<Player> tradePartner = player->tradePartner;
	if ((tradePartner && tradePartner->getTradeState() == TRADE_TRANSFER) || player->getTradeState() == TRADE_TRANSFER) {
		return;
	}

	if (player->getTradeItem()) {
		auto it = tradeItems.find(player->getTradeItem());
		if (it != tradeItems.end()) {
			tradeItems.erase(it);
		}

		player->tradeItem->onTradeEvent(ON_TRADE_CANCEL, player);
		player->tradeItem = nullptr;
	}

	player->setTradeState(TRADE_NONE);
	player->tradePartner = nullptr;

	player->sendTextMessage(MESSAGE_FAILURE, "Trade cancelled.");
	player->sendTradeClose();

	if (tradePartner) {
		if (tradePartner->getTradeItem()) {
			auto it = tradeItems.find(tradePartner->getTradeItem());
			if (it != tradeItems.end()) {
				tradeItems.erase(it);
			}

			tradePartner->tradeItem->onTradeEvent(ON_TRADE_CANCEL, tradePartner);
			tradePartner->tradeItem = nullptr;
		}

		tradePartner->setTradeState(TRADE_NONE);
		tradePartner->tradePartner = nullptr;

		tradePartner->sendTextMessage(MESSAGE_FAILURE, "Trade cancelled.");
		tradePartner->sendTradeClose();
	}
}

void Game::playerBuyItem(uint32_t playerId, uint16_t itemId, uint8_t count, uint16_t amount, bool ignoreCap /* = false*/, bool inBackpacks /* = false*/) {
	metrics::method_latency measure(__METHOD_NAME__);
	if (amount == 0) {
		return;
	}

	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	std::shared_ptr<Npc> merchant = player->getShopOwner();
	if (!merchant) {
		return;
	}

	const ItemType &it = Item::items[itemId];
	if (it.id == 0) {
		return;
	}

	if ((it.stackable && amount > 10000) || (!it.stackable && amount > 100)) {
		return;
	}

	if (!player->hasShopItemForSale(it.id, count)) {
		return;
	}

	// Check npc say exhausted
	if (player->isUIExhausted()) {
		player->sendCancelMessage(RETURNVALUE_YOUAREEXHAUSTED);
		return;
	}

	merchant->onPlayerBuyItem(player, it.id, count, amount, ignoreCap, inBackpacks);
	player->updateUIExhausted();
}

void Game::playerSellItem(uint32_t playerId, uint16_t itemId, uint8_t count, uint16_t amount, bool ignoreEquipped) {
	metrics::method_latency measure(__METHOD_NAME__);
	if (amount == 0) {
		return;
	}

	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	std::shared_ptr<Npc> merchant = player->getShopOwner();
	if (!merchant) {
		return;
	}

	const ItemType &it = Item::items[itemId];
	if (it.id == 0) {
		return;
	}

	if ((it.stackable && amount > 10000) || (!it.stackable && amount > 100)) {
		return;
	}

	// Check npc say exhausted
	if (player->isUIExhausted()) {
		player->sendCancelMessage(RETURNVALUE_YOUAREEXHAUSTED);
		return;
	}

	merchant->onPlayerSellItem(player, it.id, count, amount, ignoreEquipped);
	player->updateUIExhausted();
}

void Game::playerCloseShop(uint32_t playerId) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	player->closeShopWindow();
}

void Game::playerLookInShop(uint32_t playerId, uint16_t itemId, uint8_t count) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	std::shared_ptr<Npc> merchant = player->getShopOwner();
	if (!merchant) {
		return;
	}

	const ItemType &it = Item::items[itemId];
	if (it.id == 0) {
		return;
	}

	if (!g_events().eventPlayerOnLookInShop(player, &it, count)) {
		return;
	}

	if (!g_callbacks().checkCallback(EventCallback_t::playerOnLookInShop, &EventCallback::playerOnLookInShop, player, &it, count)) {
		return;
	}

	std::ostringstream ss;
	ss << "You see " << Item::getDescription(it, 1, nullptr, count);
	player->sendTextMessage(MESSAGE_LOOK, ss.str());
	merchant->onPlayerCheckItem(player, it.id, count);
}

void Game::playerLookAt(uint32_t playerId, uint16_t itemId, const Position &pos, uint8_t stackPos) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	std::shared_ptr<Thing> thing = internalGetThing(player, pos, stackPos, itemId, STACKPOS_LOOK);
	if (!thing) {
		player->sendCancelMessage(RETURNVALUE_NOTPOSSIBLE);
		return;
	}

	Position thingPos = thing->getPosition();
	if (!player->canSee(thingPos)) {
		player->sendCancelMessage(RETURNVALUE_NOTPOSSIBLE);
		return;
	}

	Position playerPos = player->getPosition();

	int32_t lookDistance;
	if (thing != player) {
		lookDistance = std::max<int32_t>(Position::getDistanceX(playerPos, thingPos), Position::getDistanceY(playerPos, thingPos));
		if (playerPos.z != thingPos.z) {
			lookDistance += 15;
		}
	} else {
		lookDistance = -1;
	}

	// Parse onLook from event player
	g_events().eventPlayerOnLook(player, pos, thing, stackPos, lookDistance);
	g_callbacks().executeCallback(EventCallback_t::playerOnLook, &EventCallback::playerOnLook, player, pos, thing, stackPos, lookDistance);
}

void Game::playerLookInBattleList(uint32_t playerId, uint32_t creatureId) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	std::shared_ptr<Creature> creature = getCreatureByID(creatureId);
	if (!creature) {
		return;
	}

	if (!player->canSeeCreature(creature)) {
		return;
	}

	const Position &creaturePos = creature->getPosition();
	if (!player->canSee(creaturePos)) {
		return;
	}

	int32_t lookDistance;
	if (creature != player) {
		const Position &playerPos = player->getPosition();
		lookDistance = std::max<int32_t>(Position::getDistanceX(playerPos, creaturePos), Position::getDistanceY(playerPos, creaturePos));
		if (playerPos.z != creaturePos.z) {
			lookDistance += 15;
		}
	} else {
		lookDistance = -1;
	}

	g_events().eventPlayerOnLookInBattleList(player, creature, lookDistance);
	g_callbacks().executeCallback(EventCallback_t::playerOnLookInBattleList, &EventCallback::playerOnLookInBattleList, player, creature, lookDistance);
}

void Game::playerQuickLoot(uint32_t playerId, const Position &pos, uint16_t itemId, uint8_t stackPos, std::shared_ptr<Item> defaultItem, bool lootAllCorpses, bool autoLoot) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	if (!autoLoot && !player->canDoAction()) {
		uint32_t delay = player->getNextActionTime();
		std::shared_ptr<Task> task = createPlayerTask(delay, std::bind(&Game::playerQuickLoot, this, player->getID(), pos, itemId, stackPos, defaultItem, lootAllCorpses, autoLoot), "Game::playerQuickLoot");
		player->setNextActionTask(task);
		return;
	}

	if (!autoLoot && pos.x != 0xffff) {
		if (!Position::areInRange<1, 1, 0>(pos, player->getPosition())) {
			// need to walk to the corpse first before looting it
			stdext::arraylist<Direction> listDir(128);
			if (player->getPathTo(pos, listDir, 0, 1, true, true)) {
				g_dispatcher().addEvent(std::bind(&Game::playerAutoWalk, this, player->getID(), listDir.data()), "Game::playerAutoWalk");
				std::shared_ptr<Task> task = createPlayerTask(0, std::bind(&Game::playerQuickLoot, this, player->getID(), pos, itemId, stackPos, defaultItem, lootAllCorpses, autoLoot), "Game::playerQuickLoot");
				player->setNextWalkActionTask(task);
			} else {
				player->sendCancelMessage(RETURNVALUE_THEREISNOWAY);
			}

			return;
		}
	} else if (!player->isPremium()) {
		player->sendCancelMessage("You must be premium.");
		return;
	}

	Player::PlayerLock lock(player);
	if (!autoLoot) {
		player->setNextActionTask(nullptr);
	}

	std::shared_ptr<Item> item = nullptr;
	if (!defaultItem) {
		std::shared_ptr<Thing> thing = internalGetThing(player, pos, stackPos, itemId, STACKPOS_FIND_THING);
		if (!thing) {
			player->sendCancelMessage(RETURNVALUE_NOTPOSSIBLE);
			return;
		}

		item = thing->getItem();
	} else {
		item = defaultItem;
	}

	if (!item || !item->getParent()) {
		player->sendCancelMessage(RETURNVALUE_NOTPOSSIBLE);
		return;
	}

	std::shared_ptr<Container> corpse = nullptr;
	if (pos.x == 0xffff) {
		corpse = item->getParent()->getContainer();
		if (corpse && corpse->getID() == ITEM_BROWSEFIELD) {
			corpse = item->getContainer();
			browseField = true;
		}
	} else {
		corpse = item->getContainer();
	}

	if (!corpse || corpse->hasAttribute(ItemAttribute_t::UNIQUEID) || corpse->hasAttribute(ItemAttribute_t::ACTIONID)) {
		player->sendCancelMessage(RETURNVALUE_NOTPOSSIBLE);
		return;
	}

	if (!corpse->isRewardCorpse()) {
		uint32_t corpseOwner = corpse->getCorpseOwner();
		if (corpseOwner != 0 && !player->canOpenCorpse(corpseOwner)) {
			player->sendCancelMessage(RETURNVALUE_NOTPOSSIBLE);
			return;
		}
	}

	if (pos.x == 0xffff && !browseField && !corpse->isRewardCorpse()) {
		uint32_t worth = item->getWorth();
		ObjectCategory_t category = getObjectCategory(item);
		ReturnValue ret = internalCollectLootItems(player, item, category);

		std::stringstream ss;
		if (ret == RETURNVALUE_NOTENOUGHCAPACITY) {
			ss << "Attention! The loot you are trying to pick up is too heavy for you to carry.";
		} else if (ret == RETURNVALUE_CONTAINERNOTENOUGHROOM) {
			ss << "Attention! The container for " << getObjectCategoryName(category) << " is full.";
		} else {
			if (ret == RETURNVALUE_NOERROR) {
				player->sendLootStats(item, item->getItemCount());
				ss << "You looted ";
			} else {
				ss << "You could not loot ";
			}

			if (worth != 0) {
				ss << worth << " gold.";
			} else {
				ss << "1 item.";
			}

			player->sendTextMessage(MESSAGE_LOOT, ss.str());
			return;
		}

		if (player->lastQuickLootNotification + 15000 < OTSYS_TIME()) {
			player->sendTextMessage(MESSAGE_GAME_HIGHLIGHT, ss.str());
		} else {
			player->sendTextMessage(MESSAGE_EVENT_ADVANCE, ss.str());
		}

		player->lastQuickLootNotification = OTSYS_TIME();
	} else {
		if (corpse->isRewardCorpse()) {
			auto rewardId = corpse->getAttribute<time_t>(ItemAttribute_t::DATE);
			auto reward = player->getReward(rewardId, false);
			if (reward) {
				internalQuickLootCorpse(player, reward->getContainer());
			}
		} else {
			if (!lootAllCorpses) {
				internalQuickLootCorpse(player, corpse);
			} else {
				playerLootAllCorpses(player, pos, lootAllCorpses);
			}
		}
	}
}

void Game::playerLootAllCorpses(std::shared_ptr<Player> player, const Position &pos, bool lootAllCorpses) {
	if (lootAllCorpses) {
		std::shared_ptr<Tile> tile = g_game().map.getTile(pos.x, pos.y, pos.z);
		if (!tile) {
			player->sendCancelMessage(RETURNVALUE_NOTPOSSIBLE);
			return;
		}

		const TileItemVector* itemVector = tile->getItemList();
		uint16_t corpses = 0;
		for (auto &tileItem : *itemVector) {
			if (!tileItem) {
				continue;
			}

			std::shared_ptr<Container> tileCorpse = tileItem->getContainer();
			if (!tileCorpse || !tileCorpse->isCorpse() || tileCorpse->hasAttribute(ItemAttribute_t::UNIQUEID) || tileCorpse->hasAttribute(ItemAttribute_t::ACTIONID)) {
				continue;
			}

			if (!tileCorpse->isRewardCorpse()
				&& tileCorpse->getCorpseOwner() != 0
				&& !player->canOpenCorpse(tileCorpse->getCorpseOwner())) {
				player->sendCancelMessage(RETURNVALUE_NOTPOSSIBLE);
				g_logger().debug("Player {} cannot loot corpse from id {} in position {}", player->getName(), tileItem->getID(), tileItem->getPosition().toString());
				continue;
			}

			corpses++;
			internalQuickLootCorpse(player, tileCorpse);
			if (corpses >= 30) {
				break;
			}
		}

		if (corpses > 0) {
			if (corpses > 1) {
				std::stringstream string;
				string << "You looted " << corpses << " corpses.";
				player->sendTextMessage(MESSAGE_LOOT, string.str());
			}

			return;
		}
	}

	browseField = false;
}

void Game::playerSetLootContainer(uint32_t playerId, ObjectCategory_t category, const Position &pos, uint16_t itemId, uint8_t stackPos) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player || pos.x != 0xffff) {
		return;
	}

	std::shared_ptr<Thing> thing = internalGetThing(player, pos, stackPos, itemId, STACKPOS_USEITEM);
	if (!thing) {
		player->sendCancelMessage(RETURNVALUE_NOTPOSSIBLE);
		return;
	}

	std::shared_ptr<Container> container = thing->getContainer();
	if (!container || (container->getID() == ITEM_GOLD_POUCH && category != OBJECTCATEGORY_GOLD && !g_configManager().getBoolean(TOGGLE_GOLD_POUCH_ALLOW_ANYTHING, __FUNCTION__))) {
		player->sendCancelMessage(RETURNVALUE_NOTPOSSIBLE);
		return;
	}

	if (container->getHoldingPlayer() != player) {
		player->sendCancelMessage("You must be holding the container to set it as a loot container.");
		return;
	}

	std::shared_ptr<Container> previousContainer = player->setLootContainer(category, container);
	player->sendLootContainers();

	std::shared_ptr<Cylinder> parent = container->getParent();
	if (parent) {
		parent->updateThing(container, container->getID(), container->getItemCount());
	}

	if (previousContainer != nullptr) {
		parent = previousContainer->getParent();
		if (parent) {
			parent->updateThing(previousContainer, previousContainer->getID(), previousContainer->getItemCount());
		}
	}
}

void Game::playerClearLootContainer(uint32_t playerId, ObjectCategory_t category) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	std::shared_ptr<Container> previousContainer = player->setLootContainer(category, nullptr);
	player->sendLootContainers();

	if (previousContainer != nullptr) {
		std::shared_ptr<Cylinder> parent = previousContainer->getParent();
		if (parent) {
			parent->updateThing(previousContainer, previousContainer->getID(), previousContainer->getItemCount());
		}
	}
}

void Game::playerOpenLootContainer(uint32_t playerId, ObjectCategory_t category) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	std::shared_ptr<Container> container = player->getLootContainer(category);
	if (!container) {
		return;
	}

	player->sendContainer(static_cast<uint8_t>(container->getID()), container, container->hasParent(), 0);
}

void Game::playerSetQuickLootFallback(uint32_t playerId, bool fallback) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	player->quickLootFallbackToMainContainer = fallback;
}

void Game::playerQuickLootBlackWhitelist(uint32_t playerId, QuickLootFilter_t filter, const std::vector<uint16_t> itemIds) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	player->quickLootFilter = filter;
	player->quickLootListItemIds = itemIds;
}

/*******************************************************************************
 * Depot search system
 ******************************************************************************/
void Game::playerRequestDepotItems(uint32_t playerId) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player || !player->isDepotSearchAvailable()) {
		return;
	}

	if (player->isUIExhausted(500)) {
		player->sendCancelMessage(RETURNVALUE_YOUAREEXHAUSTED);
		return;
	}

	player->requestDepotItems();
	player->updateUIExhausted();
}

void Game::playerRequestCloseDepotSearch(uint32_t playerId) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player || !player->isDepotSearchOpen()) {
		return;
	}

	player->setDepotSearchIsOpen(0, 0);
	player->sendCloseDepotSearch();
}

void Game::playerRequestDepotSearchItem(uint32_t playerId, uint16_t itemId, uint8_t tier) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player || !player->isDepotSearchOpen()) {
		return;
	}

	if (player->isUIExhausted(500)) {
		player->sendCancelMessage(RETURNVALUE_YOUAREEXHAUSTED);
		return;
	}

	player->requestDepotSearchItem(itemId, tier);
	player->updateUIExhausted();
}

void Game::playerRequestDepotSearchRetrieve(uint32_t playerId, uint16_t itemId, uint8_t tier, uint8_t type) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player || !player->isDepotSearchOpenOnItem(itemId)) {
		return;
	}

	if (player->isUIExhausted(500)) {
		player->sendCancelMessage(RETURNVALUE_YOUAREEXHAUSTED);
		return;
	}

	player->retrieveAllItemsFromDepotSearch(itemId, tier, type == 1);
	player->updateUIExhausted();
}

void Game::playerRequestOpenContainerFromDepotSearch(uint32_t playerId, const Position &pos) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player || !player->isDepotSearchOpen()) {
		return;
	}

	if (player->isUIExhausted(500)) {
		player->sendCancelMessage(RETURNVALUE_YOUAREEXHAUSTED);
		return;
	}

	player->openContainerFromDepotSearch(pos);
	player->updateUIExhausted();
}

void Game::playerCancelAttackAndFollow(uint32_t playerId) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	playerSetAttackedCreature(playerId, 0);
	playerFollowCreature(playerId, 0);
	player->stopWalk();
}

void Game::playerSetAttackedCreature(uint32_t playerId, uint32_t creatureId) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	if (player->getAttackedCreature() && creatureId == 0) {
		player->setAttackedCreature(nullptr);
		player->sendCancelTarget();
		return;
	}

	std::shared_ptr<Creature> attackCreature = getCreatureByID(creatureId);
	if (!attackCreature) {
		player->setAttackedCreature(nullptr);
		player->sendCancelTarget();
		return;
	}

	ReturnValue ret = Combat::canTargetCreature(player, attackCreature);
	if (ret != RETURNVALUE_NOERROR) {
		player->sendCancelMessage(ret);
		player->sendCancelTarget();
		player->setAttackedCreature(nullptr);
		return;
	}

	player->setAttackedCreature(attackCreature);
	g_dispatcher().addEvent(std::bind(&Game::updateCreatureWalk, this, player->getID()), "Game::updateCreatureWalk");
}

void Game::playerFollowCreature(uint32_t playerId, uint32_t creatureId) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	player->setAttackedCreature(nullptr);
	g_dispatcher().addEvent(std::bind(&Game::updateCreatureWalk, this, player->getID()), "Game::updateCreatureWalk");
	player->setFollowCreature(getCreatureByID(creatureId));
}

void Game::playerSetFightModes(uint32_t playerId, FightMode_t fightMode, bool chaseMode, bool secureMode) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	player->setFightMode(fightMode);
	player->setChaseMode(chaseMode);
	player->setSecureMode(secureMode);
}

void Game::playerRequestAddVip(uint32_t playerId, const std::string &name) {
	if (name.length() > 25) {
		return;
	}

	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	std::shared_ptr<Player> vipPlayer = getPlayerByName(name);
	if (!vipPlayer) {
		uint32_t guid;
		bool specialVip;
		std::string formattedName = name;
		if (!IOLoginData::getGuidByNameEx(guid, specialVip, formattedName)) {
			player->sendTextMessage(MESSAGE_FAILURE, "A player with this name does not exist.");
			return;
		}

		if (specialVip && !player->hasFlag(PlayerFlags_t::SpecialVIP)) {
			player->sendTextMessage(MESSAGE_FAILURE, "You can not add this player->");
			return;
		}

		player->addVIP(guid, formattedName, VIPSTATUS_OFFLINE);
	} else {
		if (vipPlayer->hasFlag(PlayerFlags_t::SpecialVIP) && !player->hasFlag(PlayerFlags_t::SpecialVIP)) {
			player->sendTextMessage(MESSAGE_FAILURE, "You can not add this player->");
			return;
		}

		if (!vipPlayer->isInGhostMode() || player->isAccessPlayer()) {
			player->addVIP(vipPlayer->getGUID(), vipPlayer->getName(), vipPlayer->statusVipList);
		} else {
			player->addVIP(vipPlayer->getGUID(), vipPlayer->getName(), VIPSTATUS_OFFLINE);
		}
	}
}

void Game::playerRequestRemoveVip(uint32_t playerId, uint32_t guid) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	player->removeVIP(guid);
}

void Game::playerRequestEditVip(uint32_t playerId, uint32_t guid, const std::string &description, uint32_t icon, bool notify) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	player->editVIP(guid, description, icon, notify);
}

void Game::playerApplyImbuement(uint32_t playerId, uint16_t imbuementid, uint8_t slot, bool protectionCharm) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	if (!player->hasImbuingItem()) {
		return;
	}

	Imbuement* imbuement = g_imbuements().getImbuement(imbuementid);
	if (!imbuement) {
		return;
	}

	std::shared_ptr<Item> item = player->imbuingItem;
	if (!item) {
		return;
	}

	if (item->getTopParent() != player) {
		g_logger().error("[Game::playerApplyImbuement] - An error occurred while player with name {} try to apply imbuement", player->getName());
		player->sendImbuementResult("An error has occurred, reopen the imbuement window. If the problem persists, contact your administrator.");
		return;
	}

	player->onApplyImbuement(imbuement, item, slot, protectionCharm);
}

void Game::playerClearImbuement(uint32_t playerid, uint8_t slot) {
	std::shared_ptr<Player> player = getPlayerByID(playerid);
	if (!player) {
		return;
	}

	if (!player->hasImbuingItem()) {
		return;
	}

	std::shared_ptr<Item> item = player->imbuingItem;
	if (!item) {
		return;
	}

	player->onClearImbuement(item, slot);
}

void Game::playerCloseImbuementWindow(uint32_t playerid) {
	std::shared_ptr<Player> player = getPlayerByID(playerid);
	if (!player) {
		return;
	}

	player->setImbuingItem(nullptr);
	return;
}

void Game::playerTurn(uint32_t playerId, Direction dir) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	if (!g_events().eventPlayerOnTurn(player, dir)) {
		return;
	}

	if (!g_callbacks().checkCallback(EventCallback_t::playerOnTurn, &EventCallback::playerOnTurn, player, dir)) {
		return;
	}

	player->resetIdleTime();
	internalCreatureTurn(player, dir);
}

void Game::playerRequestOutfit(uint32_t playerId) {
	if (!g_configManager().getBoolean(ALLOW_CHANGEOUTFIT, __FUNCTION__)) {
		return;
	}

	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	player->sendOutfitWindow();
}

void Game::playerToggleMount(uint32_t playerId, bool mount) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	player->toggleMount(mount);
}

void Game::playerChangeOutfit(uint32_t playerId, Outfit_t outfit, uint8_t isMountRandomized /* = 0*/) {
	if (!g_configManager().getBoolean(ALLOW_CHANGEOUTFIT, __FUNCTION__)) {
		return;
	}

	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	player->setRandomMount(isMountRandomized);

	if (isMountRandomized && outfit.lookMount != 0 && player->hasAnyMount()) {
		auto randomMount = mounts.getMountByID(player->getRandomMountId());
		outfit.lookMount = randomMount->clientId;
	}

	const auto playerOutfit = Outfits::getInstance().getOutfitByLookType(player->getSex(), outfit.lookType);
	if (!playerOutfit) {
		outfit.lookMount = 0;
	}

	if (outfit.lookMount != 0) {
		const auto mount = mounts.getMountByClientID(outfit.lookMount);
		if (!mount) {
			return;
		}

		if (!player->hasMount(mount)) {
			return;
		}

		std::shared_ptr<Tile> playerTile = player->getTile();
		if (!playerTile) {
			return;
		}

		if (playerTile->hasFlag(TILESTATE_PROTECTIONZONE)) {
			outfit.lookMount = 0;
		}

		auto deltaSpeedChange = mount->speed;
		if (player->isMounted()) {
			const auto prevMount = mounts.getMountByID(player->getLastMount());
			if (prevMount) {
				deltaSpeedChange -= prevMount->speed;
			}
		}

		player->setCurrentMount(mount->id);
		changeSpeed(player, deltaSpeedChange);
	} else if (player->isMounted()) {
		player->dismount();
	}

	if (player->canWear(outfit.lookType, outfit.lookAddons)) {
		player->defaultOutfit = outfit;

		if (player->hasCondition(CONDITION_OUTFIT)) {
			return;
		}

		internalCreatureChangeOutfit(player, outfit);
	}
}

void Game::playerShowQuestLog(uint32_t playerId) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	g_events().eventPlayerOnRequestQuestLog(player);
	g_callbacks().executeCallback(EventCallback_t::playerOnRequestQuestLog, &EventCallback::playerOnRequestQuestLog, player);
}

void Game::playerShowQuestLine(uint32_t playerId, uint16_t questId) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	g_events().eventPlayerOnRequestQuestLine(player, questId);
	g_callbacks().executeCallback(EventCallback_t::playerOnRequestQuestLine, &EventCallback::playerOnRequestQuestLine, player, questId);
}

void Game::playerSay(uint32_t playerId, uint16_t channelId, SpeakClasses type, const std::string &receiver, const std::string &text) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	player->resetIdleTime();

	if (playerSaySpell(player, type, text)) {
		return;
	}

	uint32_t muteTime = player->isMuted();
	if (muteTime > 0) {
		std::ostringstream ss;
		ss << "You are still muted for " << muteTime << " seconds.";
		player->sendTextMessage(MESSAGE_FAILURE, ss.str());
		return;
	}

	if (!text.empty() && text.front() == '/' && player->isAccessPlayer()) {
		return;
	}

	if (type != TALKTYPE_PRIVATE_PN) {
		player->removeMessageBuffer();
	}

	switch (type) {
		case TALKTYPE_SAY:
			internalCreatureSay(player, TALKTYPE_SAY, text, false);
			break;

		case TALKTYPE_WHISPER:
			playerWhisper(player, text);
			break;

		case TALKTYPE_YELL:
			playerYell(player, text);
			break;

		case TALKTYPE_PRIVATE_TO:
		case TALKTYPE_PRIVATE_RED_TO:
			playerSpeakTo(player, type, receiver, text);
			break;

		case TALKTYPE_CHANNEL_O:
		case TALKTYPE_CHANNEL_Y:
		case TALKTYPE_CHANNEL_R1:
			g_chat().talkToChannel(player, type, text, channelId);
			break;

		case TALKTYPE_PRIVATE_PN:
			playerSpeakToNpc(player, text);
			break;

		case TALKTYPE_BROADCAST:
			playerBroadcastMessage(player, text);
			break;

		default:
			break;
	}
}

bool Game::playerSaySpell(std::shared_ptr<Player> player, SpeakClasses type, const std::string &text) {
	if (player->walkExhausted()) {
		return true;
	}

	std::string words = text;
	TalkActionResult_t result = g_talkActions().checkPlayerCanSayTalkAction(player, type, words);
	if (result == TALKACTION_BREAK) {
		return true;
	}

	result = g_spells().playerSaySpell(player, words);
	if (result == TALKACTION_BREAK) {
		if (!g_configManager().getBoolean(PUSH_WHEN_ATTACKING, __FUNCTION__)) {
			player->cancelPush();
		}

		if (g_configManager().getBoolean(EMOTE_SPELLS, __FUNCTION__) && player->getStorageValue(STORAGEVALUE_EMOTE) == 1) {
			return internalCreatureSay(player, TALKTYPE_MONSTER_SAY, words, false);
		} else {
			return player->saySpell(type, words, false);
		}
	} else if (result == TALKACTION_FAILED) {
		return true;
	}

	return false;
}

void Game::playerWhisper(std::shared_ptr<Player> player, const std::string &text) {
	auto spectators = Spectators().find<Player>(player->getPosition(), false, MAP_MAX_CLIENT_VIEW_PORT_X, MAP_MAX_CLIENT_VIEW_PORT_X, MAP_MAX_CLIENT_VIEW_PORT_Y, MAP_MAX_CLIENT_VIEW_PORT_Y);

	// Send to client
	for (const auto &spectator : spectators) {
		if (const auto &spectatorPlayer = spectator->getPlayer()) {
			if (!Position::areInRange<1, 1>(player->getPosition(), spectatorPlayer->getPosition())) {
				spectatorPlayer->sendCreatureSay(player, TALKTYPE_WHISPER, "pspsps");
			} else {
				spectatorPlayer->sendCreatureSay(player, TALKTYPE_WHISPER, text);
			}
		}
	}

	// event method
	for (const auto &spectator : spectators) {
		spectator->onCreatureSay(player, TALKTYPE_WHISPER, text);
	}
}

bool Game::playerYell(std::shared_ptr<Player> player, const std::string &text) {
	if (player->getLevel() == 1) {
		player->sendTextMessage(MESSAGE_FAILURE, "You may not yell as long as you are on level 1.");
		return false;
	}

	if (player->hasCondition(CONDITION_YELLTICKS)) {
		player->sendCancelMessage(RETURNVALUE_YOUAREEXHAUSTED);
		return false;
	}

	if (player->getAccountType() < account::AccountType::ACCOUNT_TYPE_GAMEMASTER) {
		auto condition = Condition::createCondition(CONDITIONID_DEFAULT, CONDITION_YELLTICKS, 30000, 0);
		player->addCondition(condition);
	}

	internalCreatureSay(player, TALKTYPE_YELL, asUpperCaseString(text), false);
	return true;
}

bool Game::playerSpeakTo(std::shared_ptr<Player> player, SpeakClasses type, const std::string &receiver, const std::string &text) {
	std::shared_ptr<Player> toPlayer = getPlayerByName(receiver);
	if (!toPlayer) {
		player->sendTextMessage(MESSAGE_FAILURE, "A player with this name is not online.");
		return false;
	}

	if (type == TALKTYPE_PRIVATE_RED_TO && (player->hasFlag(PlayerFlags_t::CanTalkRedPrivate) || player->getAccountType() >= account::AccountType::ACCOUNT_TYPE_GAMEMASTER)) {
		type = TALKTYPE_PRIVATE_RED_FROM;
	} else {
		type = TALKTYPE_PRIVATE_FROM;
	}

	toPlayer->sendPrivateMessage(player, type, text);
	toPlayer->onCreatureSay(player, type, text);

	if (toPlayer->isInGhostMode() && !player->isAccessPlayer()) {
		player->sendTextMessage(MESSAGE_FAILURE, "A player with this name is not online.");
	} else {
		std::ostringstream ss;
		ss << "Message sent to " << toPlayer->getName() << '.';
		player->sendTextMessage(MESSAGE_FAILURE, ss.str());
	}
	return true;
}

void Game::playerSpeakToNpc(std::shared_ptr<Player> player, const std::string &text) {
	if (player == nullptr) {
		g_logger().error("[Game::playerSpeakToNpc] - Player is nullptr");
		return;
	}

	// Check npc say exhausted
	if (player->isUIExhausted()) {
		player->sendCancelMessage(RETURNVALUE_YOUAREEXHAUSTED);
		return;
	}

	for (const auto &spectator : Spectators().find<Creature>(player->getPosition()).filter<Npc>()) {
		spectator->getNpc()->onCreatureSay(player, TALKTYPE_PRIVATE_PN, text);
	}

	player->updateUIExhausted();
}

std::shared_ptr<Task> Game::createPlayerTask(uint32_t delay, std::function<void(void)> f, std::string context) const {
	return Player::createPlayerTask(delay, f, context);
}

//--
bool Game::canThrowObjectTo(const Position &fromPos, const Position &toPos, bool checkLineOfSight /*= true*/, int32_t rangex /*= MAP_MAX_CLIENT_VIEW_PORT_X*/, int32_t rangey /*= MAP_MAX_CLIENT_VIEW_PORT_Y*/) {
	return map.canThrowObjectTo(fromPos, toPos, checkLineOfSight, rangex, rangey);
}

bool Game::isSightClear(const Position &fromPos, const Position &toPos, bool floorCheck) {
	return map.isSightClear(fromPos, toPos, floorCheck);
}

bool Game::internalCreatureTurn(std::shared_ptr<Creature> creature, Direction dir) {
	if (creature->getDirection() == dir) {
		return false;
	}

	if (const auto &player = creature->getPlayer()) {
		player->cancelPush();
	}
	creature->setDirection(dir);

	for (const auto &spectator : Spectators().find<Player>(creature->getPosition(), true)) {
		spectator->getPlayer()->sendCreatureTurn(creature);
	}
	return true;
}

bool Game::internalCreatureSay(std::shared_ptr<Creature> creature, SpeakClasses type, const std::string &text, bool ghostMode, Spectators* spectatorsPtr /* = nullptr*/, const Position* pos /* = nullptr*/) {
	if (text.empty()) {
		return false;
	}

	if (!pos) {
		pos = &creature->getPosition();
	}

	Spectators spectators;

	if (!spectatorsPtr || spectatorsPtr->empty()) {
		// This somewhat complex construct ensures that the cached Spectators
		// is used if available and if it can be used, else a local vector is
		// used (hopefully the compiler will optimize away the construction of
		// the temporary when it's not used).
		if (type != TALKTYPE_YELL && type != TALKTYPE_MONSTER_YELL) {
			spectators.find<Creature>(*pos, false, MAP_MAX_CLIENT_VIEW_PORT_X, MAP_MAX_CLIENT_VIEW_PORT_X, MAP_MAX_CLIENT_VIEW_PORT_Y, MAP_MAX_CLIENT_VIEW_PORT_Y);
		} else {
			spectators.find<Creature>(*pos, true, (MAP_MAX_CLIENT_VIEW_PORT_X + 1) * 2, (MAP_MAX_CLIENT_VIEW_PORT_X + 1) * 2, (MAP_MAX_CLIENT_VIEW_PORT_Y + 1) * 2, (MAP_MAX_CLIENT_VIEW_PORT_Y + 1) * 2);
		}
	} else {
		spectators = (*spectatorsPtr);
	}

	// Send to client
	for (const auto &spectator : spectators) {
		if (const auto &tmpPlayer = spectator->getPlayer()) {
			if (!ghostMode || tmpPlayer->canSeeCreature(creature)) {
				tmpPlayer->sendCreatureSay(creature, type, text, pos);
			}
		}
	}

	// event method
	for (const auto &spectator : spectators) {
		spectator->onCreatureSay(creature, type, text);
		if (creature != spectator) {
			g_events().eventCreatureOnHear(spectator, creature, text, type);
			g_callbacks().executeCallback(EventCallback_t::creatureOnHear, &EventCallback::creatureOnHear, spectator, creature, text, type);
		}
	}
	return true;
}

void Game::checkCreatureWalk(uint32_t creatureId) {
	const auto &creature = getCreatureByID(creatureId);
	if (creature && creature->getHealth() > 0) {
		creature->onCreatureWalk();
		cleanup();
	}
}

void Game::updateCreatureWalk(uint32_t creatureId) {
	const auto &creature = getCreatureByID(creatureId);
	if (creature && creature->getHealth() > 0) {
		creature->goToFollowCreature_async();
	}
}

void Game::checkCreatureAttack(uint32_t creatureId) {
	const auto &creature = getCreatureByID(creatureId);
	if (creature && creature->getHealth() > 0) {
		creature->onAttacking(0);
	}
}

void Game::addCreatureCheck(const std::shared_ptr<Creature> &creature) {
	creature->creatureCheck = true;

	if (creature->inCheckCreaturesVector) {
		// already in a vector
		return;
	}

	creature->inCheckCreaturesVector = true;
	checkCreatureLists[uniform_random(0, EVENT_CREATURECOUNT - 1)].emplace_back(creature);
}

void Game::removeCreatureCheck(const std::shared_ptr<Creature> &creature) {
	metrics::method_latency measure(__METHOD_NAME__);
	if (creature->inCheckCreaturesVector) {
		creature->creatureCheck = false;
	}
}

void Game::checkCreatures() {
	metrics::method_latency measure(__METHOD_NAME__);
	static size_t index = 0;

	auto &checkCreatureList = checkCreatureLists[index];
	size_t it = 0, end = checkCreatureList.size();
	while (it < end) {
		auto creature = checkCreatureList[it];
		if (creature && creature->creatureCheck) {
			if (creature->getHealth() > 0) {
				creature->onThink(EVENT_CREATURE_THINK_INTERVAL);
				creature->onAttacking(EVENT_CREATURE_THINK_INTERVAL);
				creature->executeConditions(EVENT_CREATURE_THINK_INTERVAL);
			} else {
				afterCreatureZoneChange(creature, creature->getZones(), {});
				creature->onDeath();
			}
			++it;
		} else {
			creature->inCheckCreaturesVector = false;

			checkCreatureList[it] = checkCreatureList.back();
			checkCreatureList.pop_back();
			--end;
		}
	}
	cleanup();

	index = (index + 1) % EVENT_CREATURECOUNT;
}

void Game::changeSpeed(std::shared_ptr<Creature> creature, int32_t varSpeedDelta) {
	int32_t varSpeed = creature->getSpeed() - creature->getBaseSpeed();
	varSpeed += varSpeedDelta;

	creature->setSpeed(varSpeed);

	// Send to clients
	for (const auto &spectator : Spectators().find<Player>(creature->getPosition())) {
		spectator->getPlayer()->sendChangeSpeed(creature, creature->getStepSpeed());
	}
}

void Game::setCreatureSpeed(std::shared_ptr<Creature> creature, int32_t speed) {
	creature->setBaseSpeed(static_cast<uint16_t>(speed));

	// Send creature speed to client
	for (const auto &spectator : Spectators().find<Player>(creature->getPosition())) {
		spectator->getPlayer()->sendChangeSpeed(creature, creature->getStepSpeed());
	}
}

void Game::changePlayerSpeed(const std::shared_ptr<Player> &player, int32_t varSpeedDelta) {
	int32_t varSpeed = player->getSpeed() - player->getBaseSpeed();
	varSpeed += varSpeedDelta;

	player->setSpeed(varSpeed);

	// Send new player speed to the spectators
	for (const auto &creatureSpectator : Spectators().find<Player>(player->getPosition())) {
		creatureSpectator->getPlayer()->sendChangeSpeed(player, player->getStepSpeed());
	}
}

void Game::internalCreatureChangeOutfit(std::shared_ptr<Creature> creature, const Outfit_t &outfit) {
	if (!g_events().eventCreatureOnChangeOutfit(creature, outfit)) {
		return;
	}

	if (!g_callbacks().checkCallback(EventCallback_t::creatureOnChangeOutfit, &EventCallback::creatureOnChangeOutfit, creature, outfit)) {
		return;
	}

	creature->setCurrentOutfit(outfit);

	if (creature->isInvisible()) {
		return;
	}

	// Send to clients
	for (const auto &spectator : Spectators().find<Player>(creature->getPosition(), true)) {
		spectator->getPlayer()->sendCreatureChangeOutfit(creature, outfit);
	}
}

void Game::internalCreatureChangeVisible(std::shared_ptr<Creature> creature, bool visible) {
	// Send to clients
	for (const auto &spectator : Spectators().find<Player>(creature->getPosition(), true)) {
		spectator->getPlayer()->sendCreatureChangeVisible(creature, visible);
	}
}

void Game::changeLight(std::shared_ptr<Creature> creature) {
	// Send to clients
	for (const auto &spectator : Spectators().find<Player>(creature->getPosition(), true)) {
		spectator->getPlayer()->sendCreatureLight(creature);
	}
}

void Game::updateCreatureIcon(std::shared_ptr<Creature> creature) {
	// Send to clients
	for (const auto &spectator : Spectators().find<Player>(creature->getPosition(), true)) {
		spectator->getPlayer()->sendCreatureIcon(creature);
	}
}

void Game::reloadCreature(std::shared_ptr<Creature> creature) {
	if (!creature) {
		g_logger().error("[{}] Creature is nullptr", __FUNCTION__);
		return;
	}

	for (const auto &spectator : Spectators().find<Player>(creature->getPosition())) {
		spectator->getPlayer()->reloadCreature(creature);
	}
}

void Game::sendSingleSoundEffect(const Position &pos, SoundEffect_t soundId, std::shared_ptr<Creature> actor /* = nullptr*/) {
	if (soundId == SoundEffect_t::SILENCE) {
		return;
	}

	using enum SourceEffect_t;
	for (const auto &spectator : Spectators().find<Player>(pos)) {
		SourceEffect_t source = CREATURES;
		if (!actor || actor->getNpc()) {
			source = GLOBAL;
		} else if (actor == spectator) {
			source = OWN;
		} else if (actor->getPlayer()) {
			source = OTHERS;
		}

		spectator->getPlayer()->sendSingleSoundEffect(pos, soundId, source);
	}
}

void Game::sendDoubleSoundEffect(const Position &pos, SoundEffect_t mainSoundEffect, SoundEffect_t secondarySoundEffect, std::shared_ptr<Creature> actor /* = nullptr*/) {
	if (secondarySoundEffect == SoundEffect_t::SILENCE) {
		sendSingleSoundEffect(pos, mainSoundEffect, actor);
		return;
	}

	using enum SourceEffect_t;
	for (const auto &spectator : Spectators().find<Player>(pos)) {
		SourceEffect_t source = CREATURES;
		if (!actor || actor->getNpc()) {
			source = GLOBAL;
		} else if (actor == spectator) {
			source = OWN;
		} else if (actor->getPlayer()) {
			source = OTHERS;
		}

		spectator->getPlayer()->sendDoubleSoundEffect(pos, mainSoundEffect, source, secondarySoundEffect, source);
	}
}

bool Game::combatBlockHit(CombatDamage &damage, std::shared_ptr<Creature> attacker, std::shared_ptr<Creature> target, bool checkDefense, bool checkArmor, bool field) {
	if (damage.primary.type == COMBAT_NONE && damage.secondary.type == COMBAT_NONE) {
		return true;
	}

	if (target->getPlayer() && target->isInGhostMode()) {
		return true;
	}

	if (damage.primary.value > 0 || damage.primary.type == COMBAT_AGONYDAMAGE) {
		return false;
	}

	// Skill dodge (ruse)
	if (std::shared_ptr<Player> targetPlayer = target->getPlayer()) {
		if (auto playerArmor = targetPlayer->getInventoryItem(CONST_SLOT_ARMOR);
			playerArmor != nullptr && playerArmor->getTier()) {
			double_t chance = playerArmor->getDodgeChance();
			double_t randomChance = uniform_random(0, 10000) / 100;
			if (chance > 0 && randomChance < chance) {
				InternalGame::sendBlockEffect(BLOCK_DODGE, damage.primary.type, target->getPosition(), attacker);
				targetPlayer->sendTextMessage(MESSAGE_ATTENTION, "You dodged an attack. (Ruse)");
				return true;
			}
		}
	}

	bool canHeal = false;
	CombatDamage damageHeal;
	damageHeal.primary.type = COMBAT_HEALING;

	bool damageAbsorbMessage = false;
	bool damageIncreaseMessage = false;

	bool canReflect = false;
	CombatDamage damageReflected;
	CombatParams damageReflectedParams;

	BlockType_t primaryBlockType, secondaryBlockType;
	std::shared_ptr<Player> targetPlayer = target->getPlayer();

	if (damage.primary.type != COMBAT_NONE) {

		damage.primary.value = -damage.primary.value;
		// Damage healing primary
		if (attacker) {
			if (target->getMonster()) {
				uint32_t primaryHealing = target->getMonster()->getHealingCombatValue(damage.primary.type);
				if (primaryHealing > 0) {
					damageHeal.primary.value = std::ceil((damage.primary.value) * (primaryHealing / 100.));
					canHeal = true;
				}
			}
			if (targetPlayer && attacker->getAbsorbPercent(damage.primary.type) != 0) {
				damageAbsorbMessage = true;
			}
			if (attacker->getPlayer() && attacker->getIncreasePercent(damage.primary.type) != 0) {
				damageIncreaseMessage = true;
			}
			damage.primary.value *= attacker->getBuff(BUFF_DAMAGEDEALT) / 100.;
		}
		damage.primary.value *= target->getBuff(BUFF_DAMAGERECEIVED) / 100.;

		primaryBlockType = target->blockHit(attacker, damage.primary.type, damage.primary.value, checkDefense, checkArmor, field);

		damage.primary.value = -damage.primary.value;
		InternalGame::sendBlockEffect(primaryBlockType, damage.primary.type, target->getPosition(), attacker);
		// Damage reflection primary
		if (!damage.extension && attacker) {
			if (targetPlayer && attacker->getMonster() && damage.primary.type != COMBAT_HEALING) {
				// Charm rune (target as player)
				const auto mType = g_monsters().getMonsterType(attacker->getName());
				if (mType) {
					charmRune_t activeCharm = g_iobestiary().getCharmFromTarget(targetPlayer, mType);
					if (activeCharm == CHARM_PARRY) {
						const auto charm = g_iobestiary().getBestiaryCharm(activeCharm);
						if (charm && charm->type == CHARM_DEFENSIVE && (charm->chance > normal_random(0, 100))) {
							g_iobestiary().parseCharmCombat(charm, targetPlayer, attacker, (damage.primary.value + damage.secondary.value));
						}
					}
				}
			}
			int32_t primaryReflectPercent = target->getReflectPercent(damage.primary.type, true);
			int32_t primaryReflectFlat = target->getReflectFlat(damage.primary.type, true);
			if (primaryReflectPercent > 0 || primaryReflectFlat > 0) {
				int32_t distanceX = Position::getDistanceX(target->getPosition(), attacker->getPosition());
				int32_t distanceY = Position::getDistanceY(target->getPosition(), attacker->getPosition());
				if (target->getMonster() || damage.primary.type != COMBAT_PHYSICALDAMAGE || primaryReflectPercent > 0 || std::max(distanceX, distanceY) < 2) {
					damageReflected.primary.value = std::ceil(damage.primary.value * primaryReflectPercent / 100.) + std::max(-static_cast<int32_t>(std::ceil(attacker->getMaxHealth() * 0.01)), std::max(damage.primary.value, -(static_cast<int32_t>(primaryReflectFlat))));
					if (targetPlayer) {
						damageReflected.primary.type = COMBAT_NEUTRALDAMAGE;
					} else {
						damageReflected.primary.type = damage.primary.type;
					}
					if (!damageReflected.exString.empty()) {
						damageReflected.exString += ", ";
					}
					damageReflected.extension = true;
					damageReflected.exString += "damage reflection";
					damageReflectedParams.combatType = damage.primary.type;
					damageReflectedParams.aggressive = true;
					canReflect = true;
				}
			}
		}
	} else {
		primaryBlockType = BLOCK_NONE;
	}

	if (damage.secondary.type != COMBAT_NONE) {

		damage.secondary.value = -damage.secondary.value;
		// Damage healing secondary
		if (attacker && target->getMonster()) {
			uint32_t secondaryHealing = target->getMonster()->getHealingCombatValue(damage.secondary.type);
			if (secondaryHealing > 0) {
				;
				damageHeal.primary.value += std::ceil((damage.secondary.value) * (secondaryHealing / 100.));
				canHeal = true;
			}
			if (targetPlayer && attacker->getAbsorbPercent(damage.secondary.type) != 0) {
				damageAbsorbMessage = true;
			}
			if (attacker->getPlayer() && attacker->getIncreasePercent(damage.secondary.type) != 0) {
				damageIncreaseMessage = true;
			}
			damage.secondary.value *= attacker->getBuff(BUFF_DAMAGEDEALT) / 100.;
		}
		damage.secondary.value *= target->getBuff(BUFF_DAMAGERECEIVED) / 100.;

		secondaryBlockType = target->blockHit(attacker, damage.secondary.type, damage.secondary.value, false, false, field);

		damage.secondary.value = -damage.secondary.value;
		InternalGame::sendBlockEffect(secondaryBlockType, damage.secondary.type, target->getPosition(), attacker);

		if (!damage.extension && attacker && target->getMonster()) {
			int32_t secondaryReflectPercent = target->getReflectPercent(damage.secondary.type, true);
			int32_t secondaryReflectFlat = target->getReflectFlat(damage.secondary.type, true);
			if (secondaryReflectPercent > 0 || secondaryReflectFlat > 0) {
				if (!canReflect) {
					damageReflected.primary.type = damage.secondary.type;
					damageReflected.primary.value = std::ceil(damage.secondary.value * secondaryReflectPercent / 100.) + std::max(-static_cast<int32_t>(std::ceil(attacker->getMaxHealth() * 0.01)), std::max(damage.secondary.value, -(static_cast<int32_t>(secondaryReflectFlat))));
					if (!damageReflected.exString.empty()) {
						damageReflected.exString += ", ";
					}
					damageReflected.extension = true;
					damageReflected.exString += "damage reflection";
					damageReflectedParams.combatType = damage.primary.type;
					damageReflectedParams.aggressive = true;
					canReflect = true;
				} else {
					damageReflected.secondary.type = damage.secondary.type;
					damageReflected.primary.value = std::ceil(damage.secondary.value * secondaryReflectPercent / 100.) + std::max(-static_cast<int32_t>(std::ceil(attacker->getMaxHealth() * 0.01)), std::max(damage.secondary.value, -(static_cast<int32_t>(secondaryReflectFlat))));
				}
			}
		}
	} else {
		secondaryBlockType = BLOCK_NONE;
	}
	// Damage reflection secondary

	if (damage.primary.type == COMBAT_HEALING) {
		damage.primary.value *= target->getBuff(BUFF_HEALINGRECEIVED) / 100.;
	}

	if (damageAbsorbMessage) {
		if (!damage.exString.empty()) {
			damage.exString += ", ";
		}
		damage.exString += "active elemental resiliance";
	}

	if (damageIncreaseMessage) {
		if (!damage.exString.empty()) {
			damage.exString += ", ";
		}
		damage.exString += "active elemental amplification";
	}

	if (canReflect) {
		Combat::doCombatHealth(target, attacker, damageReflected, damageReflectedParams);
	}
	if (canHeal) {
		combatChangeHealth(nullptr, target, damageHeal);
	}
	return (primaryBlockType != BLOCK_NONE) && (secondaryBlockType != BLOCK_NONE);
}

void Game::combatGetTypeInfo(CombatType_t combatType, std::shared_ptr<Creature> target, TextColor_t &color, uint16_t &effect) {
	switch (combatType) {
		case COMBAT_PHYSICALDAMAGE: {
			std::shared_ptr<Item> splash = nullptr;
			switch (target->getRace()) {
				case RACE_VENOM:
					color = TEXTCOLOR_LIGHTGREEN;
					effect = CONST_ME_HITBYPOISON;
					splash = Item::CreateItem(ITEM_SMALLSPLASH, FLUID_SLIME);
					break;
				case RACE_BLOOD:
					color = TEXTCOLOR_RED;
					effect = CONST_ME_DRAWBLOOD;
					if (std::shared_ptr<Tile> tile = target->getTile()) {
						if (!tile->hasFlag(TILESTATE_PROTECTIONZONE)) {
							splash = Item::CreateItem(ITEM_SMALLSPLASH, FLUID_BLOOD);
						}
					}
					break;
				case RACE_INK:
					color = TEXTCOLOR_LIGHTGREY;
					effect = CONST_ME_HITAREA;
					splash = Item::CreateItem(ITEM_SMALLSPLASH, FLUID_INK);
					break;
				case RACE_UNDEAD:
					color = TEXTCOLOR_LIGHTGREY;
					effect = CONST_ME_HITAREA;
					break;
				case RACE_FIRE:
					color = TEXTCOLOR_ORANGE;
					effect = CONST_ME_DRAWBLOOD;
					break;
				case RACE_ENERGY:
					color = TEXTCOLOR_PURPLE;
					effect = CONST_ME_ENERGYHIT;
					break;
				default:
					color = TEXTCOLOR_NONE;
					effect = CONST_ME_NONE;
					break;
			}

			if (splash) {
				internalAddItem(target->getTile(), splash, INDEX_WHEREEVER, FLAG_NOLIMIT);
				splash->startDecaying();
			}

			break;
		}

		case COMBAT_ENERGYDAMAGE: {
			color = TEXTCOLOR_PURPLE;
			effect = CONST_ME_ENERGYHIT;
			break;
		}

		case COMBAT_EARTHDAMAGE: {
			color = TEXTCOLOR_LIGHTGREEN;
			effect = CONST_ME_GREEN_RINGS;
			break;
		}

		case COMBAT_DROWNDAMAGE: {
			color = TEXTCOLOR_LIGHTBLUE;
			effect = CONST_ME_LOSEENERGY;
			break;
		}
		case COMBAT_FIREDAMAGE: {
			color = TEXTCOLOR_ORANGE;
			effect = CONST_ME_HITBYFIRE;
			break;
		}
		case COMBAT_ICEDAMAGE: {
			color = TEXTCOLOR_SKYBLUE;
			effect = CONST_ME_ICEATTACK;
			break;
		}
		case COMBAT_HOLYDAMAGE: {
			color = TEXTCOLOR_YELLOW;
			effect = CONST_ME_HOLYDAMAGE;
			break;
		}
		case COMBAT_DEATHDAMAGE: {
			color = TEXTCOLOR_DARKRED;
			effect = CONST_ME_SMALLCLOUDS;
			break;
		}
		case COMBAT_LIFEDRAIN: {
			color = TEXTCOLOR_RED;
			effect = CONST_ME_MAGIC_RED;
			break;
		}
		case COMBAT_AGONYDAMAGE: {
			color = TEXTCOLOR_DARKBROWN;
			effect = CONST_ME_AGONY;
			break;
		}
		case COMBAT_NEUTRALDAMAGE: {
			color = TEXTCOLOR_NEUTRALDAMAGE;
			effect = CONST_ME_REDSMOKE;
			break;
		}
		default: {
			color = TEXTCOLOR_NONE;
			effect = CONST_ME_NONE;
			break;
		}
	}
}

// Hazard combat helpers
void Game::handleHazardSystemAttack(CombatDamage &damage, std::shared_ptr<Player> player, std::shared_ptr<Monster> monster, bool isPlayerAttacker) {
	if (damage.primary.value != 0 && monster->getHazard()) {
		if (isPlayerAttacker) {
			player->parseAttackDealtHazardSystem(damage, monster);
		} else {
			player->parseAttackRecvHazardSystem(damage, monster);
		}
	}
}

void Game::notifySpectators(const CreatureVector &spectators, const Position &targetPos, std::shared_ptr<Player> attackerPlayer, std::shared_ptr<Monster> targetMonster) {
	if (!spectators.empty()) {
		for (auto spectator : spectators) {
			if (!spectator) {
				continue;
			}

			const auto tmpPlayer = spectator->getPlayer();
			if (!tmpPlayer || tmpPlayer->getPosition().z != targetPos.z) {
				continue;
			}

			std::stringstream ss;
			ss << ucfirst(targetMonster->getNameDescription()) << " has dodged";
			if (tmpPlayer == attackerPlayer) {
				ss << " your attack.";
				attackerPlayer->sendCancelMessage(ss.str());
				ss << " (Hazard)";
				attackerPlayer->sendTextMessage(MESSAGE_DAMAGE_OTHERS, ss.str());
			} else {
				ss << " an attack by " << attackerPlayer->getName() << ". (Hazard)";
				tmpPlayer->sendTextMessage(MESSAGE_DAMAGE_OTHERS, ss.str());
			}
		}
		addMagicEffect(targetPos, CONST_ME_DODGE);
	}
}

// Custom PvP System combat helpers
void Game::applyPvPDamage(CombatDamage &damage, std::shared_ptr<Player> attacker, std::shared_ptr<Player> target) {
	float targetDamageReceivedMultiplier = target->vocation->pvpDamageReceivedMultiplier;
	float attackerDamageDealtMultiplier = attacker->vocation->pvpDamageDealtMultiplier;
	float levelDifferenceDamageMultiplier = this->pvpLevelDifferenceDamageMultiplier(attacker, target);

	float pvpDamageMultiplier = targetDamageReceivedMultiplier * attackerDamageDealtMultiplier * levelDifferenceDamageMultiplier;

	damage.primary.value = std::round(damage.primary.value * pvpDamageMultiplier);
	damage.secondary.value = std::round(damage.secondary.value * pvpDamageMultiplier);
}

float Game::pvpLevelDifferenceDamageMultiplier(std::shared_ptr<Player> attacker, std::shared_ptr<Player> target) {
	int32_t levelDifference = target->getLevel() - attacker->getLevel();
	levelDifference = std::abs(levelDifference);
	bool isLowerLevel = target->getLevel() < attacker->getLevel();

	int32_t maxLevelDifference = g_configManager().getNumber(PVP_MAX_LEVEL_DIFFERENCE, __FUNCTION__);
	levelDifference = std::min(levelDifference, maxLevelDifference);

	float levelDiffRate = 1.0;
	if (isLowerLevel) {
		float rateDamageTakenByLevel = g_configManager().getFloat(PVP_RATE_DAMAGE_TAKEN_PER_LEVEL, __FUNCTION__) / 100;
		levelDiffRate += levelDifference * rateDamageTakenByLevel;
	} else {
		float rateDamageReductionByLevel = g_configManager().getFloat(PVP_RATE_DAMAGE_REDUCTION_PER_LEVEL, __FUNCTION__) / 100;
		levelDiffRate -= levelDifference * rateDamageReductionByLevel;
	}

	return levelDiffRate;
}

// Wheel of destiny combat helpers
void Game::applyWheelOfDestinyHealing(CombatDamage &damage, std::shared_ptr<Player> attackerPlayer, std::shared_ptr<Creature> target) {
	damage.primary.value += (damage.primary.value * damage.healingMultiplier) / 100.;

	if (attackerPlayer) {
		damage.primary.value += attackerPlayer->wheel()->getStat(WheelStat_t::HEALING);

		if (damage.secondary.value != 0) {
			damage.secondary.value += attackerPlayer->wheel()->getStat(WheelStat_t::HEALING);
		}

		if (damage.healingLink > 0) {
			CombatDamage tmpDamage;
			tmpDamage.primary.value = (damage.primary.value * damage.healingLink) / 100;
			tmpDamage.primary.type = COMBAT_HEALING;
			combatChangeHealth(attackerPlayer, attackerPlayer, tmpDamage);
		}

		if (attackerPlayer->wheel()->getInstant("Blessing of the Grove")) {
			damage.primary.value += (damage.primary.value * attackerPlayer->wheel()->checkBlessingGroveHealingByTarget(target)) / 100.;
		}
	}
}

void Game::applyWheelOfDestinyEffectsToDamage(CombatDamage &damage, std::shared_ptr<Player> attackerPlayer, std::shared_ptr<Creature> target) const {
	// If damage is 0, it means the target is immune to the damage type, or that we missed.
	if (damage.primary.value == 0 && damage.secondary.value == 0) {
		return;
	}

	if (damage.damageMultiplier > 0) {
		damage.primary.value += (damage.primary.value * (damage.damageMultiplier)) / 100.;
		damage.secondary.value += (damage.secondary.value * (damage.damageMultiplier)) / 100.;
	}

	if (attackerPlayer) {
		damage.primary.value -= attackerPlayer->wheel()->getStat(WheelStat_t::DAMAGE);
		if (damage.secondary.value != 0) {
			damage.secondary.value -= attackerPlayer->wheel()->getStat(WheelStat_t::DAMAGE);
		}
		if (damage.instantSpellName == "Twin Burst") {
			int32_t damageBonus = attackerPlayer->wheel()->checkTwinBurstByTarget(target);
			if (damageBonus != 0) {
				damage.primary.value += (damage.primary.value * damageBonus) / 100.;
				damage.secondary.value += (damage.secondary.value * damageBonus) / 100.;
			}
		}
		if (damage.instantSpellName == "Executioner's Throw") {
			int32_t damageBonus = attackerPlayer->wheel()->checkExecutionersThrow(target);
			if (damageBonus != 0) {
				damage.primary.value += (damage.primary.value * damageBonus) / 100.;
				damage.secondary.value += (damage.secondary.value * damageBonus) / 100.;
			}
		}
		if (damage.instantSpellName == "Divine Grenade") {
			int32_t damageBonus = attackerPlayer->wheel()->checkDivineGrenade(target);
			if (damageBonus != 0) {
				damage.primary.value += (damage.primary.value * damageBonus) / 100.;
				damage.secondary.value += (damage.secondary.value * damageBonus) / 100.;
			}
		}
	}
}

int32_t Game::applyHealthChange(CombatDamage &damage, std::shared_ptr<Creature> target) const {
	int32_t targetHealth = target->getHealth();

	// Wheel of destiny (Gift of Life)
	if (std::shared_ptr<Player> targetPlayer = target->getPlayer()) {
		if (targetPlayer->wheel()->getInstant("Gift of Life") && targetPlayer->wheel()->getGiftOfCooldown() == 0 && (damage.primary.value + damage.secondary.value) >= targetHealth) {
			int32_t overkillMultiplier = (damage.primary.value + damage.secondary.value) - targetHealth;
			overkillMultiplier = (overkillMultiplier * 100) / targetPlayer->getMaxHealth();
			if (overkillMultiplier <= targetPlayer->wheel()->getGiftOfLifeValue()) {
				targetPlayer->wheel()->checkGiftOfLife();
				targetHealth = target->getHealth();
			}
		}
	}

	if (damage.primary.value >= targetHealth) {
		damage.primary.value = targetHealth;
		damage.secondary.value = 0;
	} else if (damage.secondary.value) {
		damage.secondary.value = std::min<int32_t>(damage.secondary.value, targetHealth - damage.primary.value);
	}

	return targetHealth;
}

bool Game::combatChangeHealth(std::shared_ptr<Creature> attacker, std::shared_ptr<Creature> target, CombatDamage &damage, bool isEvent /*= false*/) {
	using namespace std;
	const Position &targetPos = target->getPosition();
	if (damage.primary.value > 0) {
		if (target->getHealth() <= 0) {
			return false;
		}

		std::shared_ptr<Player> attackerPlayer;
		if (attacker) {
			attackerPlayer = attacker->getPlayer();
		} else {
			attackerPlayer = nullptr;
		}

		auto targetPlayer = target->getPlayer();
		if (attackerPlayer && targetPlayer && attackerPlayer->getSkull() == SKULL_BLACK && attackerPlayer->getSkullClient(targetPlayer) == SKULL_NONE) {
			return false;
		}

		if (damage.origin != ORIGIN_NONE) {
			const auto events = target->getCreatureEvents(CREATURE_EVENT_HEALTHCHANGE);
			if (!events.empty()) {
				for (const auto creatureEvent : events) {
					creatureEvent->executeHealthChange(target, attacker, damage);
				}
				damage.origin = ORIGIN_NONE;
				return combatChangeHealth(attacker, target, damage);
			}
		}

		// Wheel of destiny combat healing
		applyWheelOfDestinyHealing(damage, attackerPlayer, target);

		auto realHealthChange = target->getHealth();
		target->gainHealth(attacker, damage.primary.value);
		realHealthChange = target->getHealth() - realHealthChange;

		if (realHealthChange > 0 && !target->isInGhostMode()) {
			if (targetPlayer) {
				targetPlayer->updateImpactTracker(COMBAT_HEALING, realHealthChange);
			}

			// Party hunt analyzer
			if (auto party = attackerPlayer ? attackerPlayer->getParty() : nullptr) {
				party->addPlayerHealing(attackerPlayer, realHealthChange);
			}

			std::stringstream ss;

			ss << realHealthChange << (realHealthChange != 1 ? " hitpoints." : " hitpoint.");
			std::string damageString = ss.str();

			std::string spectatorMessage;

			TextMessage message;
			message.position = targetPos;
			message.primary.value = realHealthChange;
			message.primary.color = TEXTCOLOR_PASTELRED;

			for (const auto &spectator : Spectators().find<Player>(targetPos)) {
				const auto &tmpPlayer = spectator->getPlayer();
				if (!tmpPlayer) {
					continue;
				}

				if (tmpPlayer == attackerPlayer && attackerPlayer != targetPlayer) {
					ss.str({});
					ss << "You heal " << target->getNameDescription() << " for " << damageString;
					message.type = MESSAGE_HEALED;
					message.text = ss.str();
				} else if (tmpPlayer == targetPlayer) {
					ss.str({});
					if (!attacker) {
						ss << "You were healed";
					} else if (targetPlayer == attackerPlayer) {
						ss << "You heal yourself";
					} else {
						ss << "You were healed by " << attacker->getNameDescription();
					}
					ss << " for " << damageString;
					message.type = MESSAGE_HEALED;
					message.text = ss.str();
				} else {
					if (spectatorMessage.empty()) {
						ss.str({});
						if (!attacker) {
							ss << ucfirst(target->getNameDescription()) << " was healed";
						} else {
							ss << ucfirst(attacker->getNameDescription()) << " healed ";
							if (attacker == target) {
								ss << (targetPlayer ? targetPlayer->getReflexivePronoun() : "itself");
							} else {
								ss << target->getNameDescription();
							}
						}
						ss << " for " << damageString;
						spectatorMessage = ss.str();
					}
					message.type = MESSAGE_HEALED_OTHERS;
					message.text = spectatorMessage;
				}
				tmpPlayer->sendTextMessage(message);
			}
		}
	} else {
		if (!target->isAttackable()) {
			if (!target->isInGhostMode()) {
				addMagicEffect(targetPos, CONST_ME_POFF);
			}
			return true;
		}

		const auto &attackerPlayer = attacker ? attacker->getPlayer() : nullptr;

		const auto &targetPlayer = target->getPlayer();
		if (attackerPlayer && targetPlayer && attackerPlayer->getSkull() == SKULL_BLACK && attackerPlayer->getSkullClient(targetPlayer) == SKULL_NONE) {
			return false;
		}

		// Wheel of destiny apply combat effects
		applyWheelOfDestinyEffectsToDamage(damage, attackerPlayer, target);

		damage.primary.value = std::abs(damage.primary.value);
		damage.secondary.value = std::abs(damage.secondary.value);

		std::shared_ptr<Monster> targetMonster;
		if (target && target->getMonster()) {
			targetMonster = target->getMonster();
		} else {
			targetMonster = nullptr;
		}

		std::shared_ptr<Monster> attackerMonster;
		if (attacker && attacker->getMonster()) {
			attackerMonster = attacker->getMonster();
		} else {
			attackerMonster = nullptr;
		}

		if (attacker && attackerPlayer && damage.extension == false && damage.origin == ORIGIN_RANGED && target == attackerPlayer->getAttackedCreature()) {
			const Position &attackerPos = attacker->getPosition();
			if (targetPos.z == attackerPos.z) {
				int32_t distanceX = Position::getDistanceX(targetPos, attackerPos);
				int32_t distanceY = Position::getDistanceY(targetPos, attackerPos);
				int32_t damageX = attackerPlayer->getPerfectShotDamage(distanceX, true);
				int32_t damageY = attackerPlayer->getPerfectShotDamage(distanceY, true);
				std::shared_ptr<Item> item = attackerPlayer->getWeapon();
				if (item && item->getWeaponType() == WEAPON_DISTANCE) {
					std::shared_ptr<Item> quiver = attackerPlayer->getInventoryItem(CONST_SLOT_RIGHT);
					if (quiver && quiver->getWeaponType()) {
						if (quiver->getPerfectShotRange() == distanceX) {
							damageX -= quiver->getPerfectShotDamage();
						} else if (quiver->getPerfectShotRange() == distanceY) {
							damageY -= quiver->getPerfectShotDamage();
						}
					}
				}
				if (damageX != 0 || damageY != 0) {
					int32_t totalDamage = damageX;
					if (distanceX != distanceY) {
						totalDamage += damageY;
					}
					damage.primary.value += totalDamage;
					if (!damage.exString.empty()) {
						damage.exString += ", ";
					}
					damage.exString += "perfect shot";
				}
			}
		}

		TextMessage message;
		message.position = targetPos;

		if (!isEvent) {
			g_events().eventCreatureOnDrainHealth(target, attacker, damage.primary.type, damage.primary.value, damage.secondary.type, damage.secondary.value, message.primary.color, message.secondary.color);
			g_callbacks().executeCallback(EventCallback_t::creatureOnDrainHealth, &EventCallback::creatureOnDrainHealth, target, attacker, damage.primary.type, damage.primary.value, damage.secondary.type, damage.secondary.value, message.primary.color, message.secondary.color);
		}
		if (damage.origin != ORIGIN_NONE && attacker && damage.primary.type != COMBAT_HEALING) {
			damage.primary.value *= attacker->getBuff(BUFF_DAMAGEDEALT) / 100.;
			damage.secondary.value *= attacker->getBuff(BUFF_DAMAGEDEALT) / 100.;
		}
		if (damage.origin != ORIGIN_NONE && target && damage.primary.type != COMBAT_HEALING) {
			damage.primary.value *= target->getBuff(BUFF_DAMAGERECEIVED) / 100.;
			damage.secondary.value *= target->getBuff(BUFF_DAMAGERECEIVED) / 100.;
		}
		auto healthChange = damage.primary.value + damage.secondary.value;
		if (healthChange == 0) {
			return true;
		}

		auto spectators = Spectators().find<Player>(targetPos, true);

		if (targetPlayer && attackerMonster) {
			handleHazardSystemAttack(damage, targetPlayer, attackerMonster, false);
		} else if (attackerPlayer && targetMonster) {
			handleHazardSystemAttack(damage, attackerPlayer, targetMonster, true);

			if (damage.primary.value == 0 && damage.secondary.value == 0) {
				notifySpectators(spectators.data(), targetPos, attackerPlayer, targetMonster);
				return true;
			}
		}

		if (damage.fatal) {
			addMagicEffect(spectators.data(), targetPos, CONST_ME_FATAL);
		} else if (damage.critical) {
			addMagicEffect(spectators.data(), targetPos, CONST_ME_CRITICAL_DAMAGE);
		}

		if (!damage.extension && attackerMonster && targetPlayer) {
			// Charm rune (target as player)
			if (charmRune_t activeCharm = g_iobestiary().getCharmFromTarget(targetPlayer, g_monsters().getMonsterTypeByRaceId(attackerMonster->getRaceId()));
				activeCharm != CHARM_NONE && activeCharm != CHARM_CLEANSE) {
				if (const auto charm = g_iobestiary().getBestiaryCharm(activeCharm);
					charm->type == CHARM_DEFENSIVE && charm->chance > normal_random(0, 100) && g_iobestiary().parseCharmCombat(charm, targetPlayer, attacker, (damage.primary.value + damage.secondary.value))) {
					return false; // Dodge charm
				}
			}
		}

		std::string attackMsg = fmt::format("{} attack", damage.critical ? "critical " : " ");
		std::stringstream ss;

		if (target->hasCondition(CONDITION_MANASHIELD) && damage.primary.type != COMBAT_UNDEFINEDDAMAGE) {
			int32_t manaDamage = std::min<int32_t>(target->getMana(), healthChange);
			uint16_t manaShield = target->getManaShield();
			if (manaShield > 0) {
				if (manaShield > manaDamage) {
					target->setManaShield(manaShield - manaDamage);
					manaShield = manaShield - manaDamage;
				} else {
					manaDamage = manaShield;
					target->removeCondition(CONDITION_MANASHIELD);
					manaShield = 0;
				}
			}
			if (manaDamage != 0) {
				if (damage.origin != ORIGIN_NONE) {
					const auto events = target->getCreatureEvents(CREATURE_EVENT_MANACHANGE);
					if (!events.empty()) {
						for (const auto creatureEvent : events) {
							creatureEvent->executeManaChange(target, attacker, damage);
						}
						healthChange = damage.primary.value + damage.secondary.value;
						if (healthChange == 0) {
							return true;
						}
						manaDamage = std::min<int32_t>(target->getMana(), healthChange);
					}
				}

				target->drainMana(attacker, manaDamage);

				if (target->getMana() == 0 && manaShield > 0) {
					target->removeCondition(CONDITION_MANASHIELD);
				}

				addMagicEffect(spectators.data(), targetPos, CONST_ME_LOSEENERGY);

				std::string damageString = std::to_string(manaDamage);

				std::string spectatorMessage;

				message.primary.value = manaDamage;
				message.primary.color = TEXTCOLOR_BLUE;

				for (const auto &spectator : spectators) {
					const auto &tmpPlayer = spectator->getPlayer();
					if (!tmpPlayer || tmpPlayer->getPosition().z != targetPos.z) {
						continue;
					}

					if (tmpPlayer == attackerPlayer && attackerPlayer != targetPlayer) {
						ss.str({});
						ss << ucfirst(target->getNameDescription()) << " loses " << damageString + " mana due to your " << attackMsg << ".";

						if (!damage.exString.empty()) {
							ss << " (" << damage.exString << ")";
						}
						message.type = MESSAGE_DAMAGE_DEALT;
						message.text = ss.str();
					} else if (tmpPlayer == targetPlayer) {
						ss.str({});
						ss << "You lose " << damageString << " mana";
						if (!attacker) {
							ss << '.';
						} else if (targetPlayer == attackerPlayer) {
							ss << " due to your own " << attackMsg << ".";
						} else {
							ss << " due to an " << attackMsg << " by " << attacker->getNameDescription() << '.';
						}
						message.type = MESSAGE_DAMAGE_RECEIVED;
						message.text = ss.str();
					} else {
						if (spectatorMessage.empty()) {
							ss.str({});
							ss << ucfirst(target->getNameDescription()) << " loses " << damageString + " mana";
							if (attacker) {
								ss << " due to ";
								if (attacker == target) {
									ss << (targetPlayer ? targetPlayer->getPossessivePronoun() : "its") << " own attack";
								} else {
									ss << "an " << attackMsg << " by " << attacker->getNameDescription();
								}
							}
							ss << '.';
							spectatorMessage = ss.str();
						}
						message.type = MESSAGE_DAMAGE_OTHERS;
						message.text = spectatorMessage;
					}
					tmpPlayer->sendTextMessage(message);
				}

				damage.primary.value -= manaDamage;
				if (damage.primary.value < 0) {
					damage.secondary.value = std::max<int32_t>(0, damage.secondary.value + damage.primary.value);
					damage.primary.value = 0;
				}

				if (attackerPlayer) {
					attackerPlayer->updateImpactTracker(damage.primary.type, damage.primary.value);
					if (damage.secondary.type != COMBAT_NONE) {
						attackerPlayer->updateImpactTracker(damage.secondary.type, damage.secondary.value);
					}
				}

				if (targetPlayer) {
					targetPlayer->updateImpactTracker(damage.primary.type, manaDamage);
					if (damage.secondary.type != COMBAT_NONE) {
						targetPlayer->updateImpactTracker(damage.secondary.type, damage.secondary.value);
					}
				}
			}
		}

		auto realDamage = damage.primary.value + damage.secondary.value;
		if (realDamage == 0) {
			return true;
		}

		if (damage.origin != ORIGIN_NONE) {
			const auto events = target->getCreatureEvents(CREATURE_EVENT_HEALTHCHANGE);
			if (!events.empty()) {
				for (const auto creatureEvent : events) {
					creatureEvent->executeHealthChange(target, attacker, damage);
				}
				damage.origin = ORIGIN_NONE;
				return combatChangeHealth(attacker, target, damage);
			}
		}

		auto targetHealth = applyHealthChange(damage, target);
		if (damage.primary.value >= targetHealth) {
			damage.primary.value = targetHealth;
			damage.secondary.value = 0;
		} else if (damage.secondary.value) {
			damage.secondary.value = std::min<int32_t>(damage.secondary.value, targetHealth - damage.primary.value);
		}

		// Apply Custom PvP Damage (must be placed here to avoid recursive calls)
		if (attackerPlayer && targetPlayer) {
			applyPvPDamage(damage, attackerPlayer, targetPlayer);
		}

		realDamage = damage.primary.value + damage.secondary.value;
		if (realDamage == 0) {
			return true;
		} else if (realDamage >= targetHealth) {
			for (const auto creatureEvent : target->getCreatureEvents(CREATURE_EVENT_PREPAREDEATH)) {
				if (!creatureEvent->executeOnPrepareDeath(target, attacker)) {
					return false;
				}
			}
		}

		target->drainHealth(attacker, realDamage);
		if (realDamage > 0 && targetMonster) {
			if (attackerPlayer && attackerPlayer->getPlayer()) {
				attackerPlayer->updateImpactTracker(damage.secondary.type, damage.secondary.value);
			}

			if (targetMonster->israndomStepping()) {
				targetMonster->setIgnoreFieldDamage(true);
				targetMonster->updateMapCache();
			}
		}

		if (spectators.empty()) {
			spectators.find<Player>(targetPos, true);
		}

		addCreatureHealth(spectators.data(), target);

		sendDamageMessageAndEffects(
			attacker,
			target,
			damage,
			targetPos,
			attackerPlayer,
			targetPlayer,
			message,
			spectators.data(),
			realDamage
		);

		if (attackerPlayer) {
			if (!damage.extension && damage.origin != ORIGIN_CONDITION) {
				applyCharmRune(targetMonster, attackerPlayer, target, realDamage);
				applyLifeLeech(attackerPlayer, targetMonster, target, damage, realDamage);
				applyManaLeech(attackerPlayer, targetMonster, target, damage, realDamage);
			}
			updatePlayerPartyHuntAnalyzer(damage, attackerPlayer);
		}
	}

	return true;
}

void Game::updatePlayerPartyHuntAnalyzer(const CombatDamage &damage, std::shared_ptr<Player> player) const {
	if (!player) {
		return;
	}

	if (auto party = player->getParty()) {
		if (damage.primary.value != 0) {
			party->addPlayerDamage(player, damage.primary.value);
		}
		if (damage.secondary.value != 0) {
			party->addPlayerDamage(player, damage.secondary.value);
		}
	}
}

void Game::sendDamageMessageAndEffects(
	std::shared_ptr<Creature> attacker, std::shared_ptr<Creature> target, const CombatDamage &damage,
	const Position &targetPos, std::shared_ptr<Player> attackerPlayer, std::shared_ptr<Player> targetPlayer,
	TextMessage &message, const CreatureVector &spectators, int32_t realDamage
) {
	message.primary.value = damage.primary.value;
	message.secondary.value = damage.secondary.value;

	sendEffects(target, damage, targetPos, message, spectators);

	if (shouldSendMessage(message)) {
		sendMessages(attacker, target, damage, targetPos, attackerPlayer, targetPlayer, message, spectators, realDamage);
	}
}

bool Game::shouldSendMessage(const TextMessage &message) const {
	return message.primary.color != TEXTCOLOR_NONE || message.secondary.color != TEXTCOLOR_NONE;
}

void Game::sendMessages(
	std::shared_ptr<Creature> attacker, std::shared_ptr<Creature> target, const CombatDamage &damage,
	const Position &targetPos, std::shared_ptr<Player> attackerPlayer, std::shared_ptr<Player> targetPlayer,
	TextMessage &message, const CreatureVector &spectators, int32_t realDamage
) const {
	if (attackerPlayer) {
		attackerPlayer->updateImpactTracker(damage.primary.type, damage.primary.value);
		if (damage.secondary.type != COMBAT_NONE) {
			attackerPlayer->updateImpactTracker(damage.secondary.type, damage.secondary.value);
		}
	}
	if (targetPlayer) {
		std::string cause = "(other)";
		if (attacker) {
			cause = attacker->getName();
		}

		targetPlayer->updateInputAnalyzer(damage.primary.type, damage.primary.value, cause);
		if (attackerPlayer) {
			if (damage.secondary.type != COMBAT_NONE) {
				attackerPlayer->updateInputAnalyzer(damage.secondary.type, damage.secondary.value, cause);
			}
		}
	}
	std::stringstream ss;

	ss << realDamage << (realDamage != 1 ? " hitpoints" : " hitpoint");
	std::string damageString = ss.str();

	std::string spectatorMessage;

	for (std::shared_ptr<Creature> spectator : spectators) {
		std::shared_ptr<Player> tmpPlayer = spectator->getPlayer();
		if (!tmpPlayer || tmpPlayer->getPosition().z != targetPos.z) {
			continue;
		}

		if (tmpPlayer == attackerPlayer && attackerPlayer != targetPlayer) {
			buildMessageAsAttacker(target, damage, message, ss, damageString);
		} else if (tmpPlayer == targetPlayer) {
			buildMessageAsTarget(attacker, damage, attackerPlayer, targetPlayer, message, ss, damageString);
		} else {
			buildMessageAsSpectator(attacker, target, damage, targetPlayer, message, ss, damageString, spectatorMessage);
		}
		tmpPlayer->sendTextMessage(message);
	}
}

void Game::buildMessageAsSpectator(
	std::shared_ptr<Creature> attacker, std::shared_ptr<Creature> target, const CombatDamage &damage,
	std::shared_ptr<Player> targetPlayer, TextMessage &message, std::stringstream &ss,
	const std::string &damageString, std::string &spectatorMessage
) const {
	if (spectatorMessage.empty()) {
		ss.str({});
		auto attackMsg = damage.critical ? "critical " : "";
		auto article = damage.critical ? "a" : "an";
		ss << ucfirst(target->getNameDescription()) << " loses " << damageString;
		if (attacker) {
			ss << " due to ";
			if (attacker == target) {
				if (targetPlayer) {
					ss << targetPlayer->getPossessivePronoun() << " own " << attackMsg << "attack";
				} else {
					ss << "its own " << attackMsg << "attack";
				}
			} else {
				ss << article << " " << attackMsg << "attack by " << attacker->getNameDescription();
			}
		}
		ss << '.';
		if (damage.extension) {
			ss << " " << damage.exString;
		}
		spectatorMessage = ss.str();
	}

	message.type = MESSAGE_DAMAGE_OTHERS;
	message.text = spectatorMessage;
}

void Game::buildMessageAsTarget(
	std::shared_ptr<Creature> attacker, const CombatDamage &damage, std::shared_ptr<Player> attackerPlayer,
	std::shared_ptr<Player> targetPlayer, TextMessage &message, std::stringstream &ss,
	const std::string &damageString
) const {
	ss.str({});
	auto attackMsg = damage.critical ? "critical " : "";
	auto article = damage.critical ? "a" : "an";
	ss << "You lose " << damageString;
	if (!attacker) {
		ss << '.';
	} else if (targetPlayer == attackerPlayer) {
		ss << " due to your own " << attackMsg << "attack.";
	} else {
		ss << " due to " << article << " " << attackMsg << "attack by " << attacker->getNameDescription() << '.';
	}
	if (damage.extension) {
		ss << " " << damage.exString;
	}
	message.type = MESSAGE_DAMAGE_RECEIVED;
	message.text = ss.str();
}

void Game::buildMessageAsAttacker(
	std::shared_ptr<Creature> target, const CombatDamage &damage, TextMessage &message,
	std::stringstream &ss, const std::string &damageString
) const {
	ss.str({});
	ss << ucfirst(target->getNameDescription()) << " loses " << damageString << " due to your " << (damage.critical ? "critical " : " ") << "attack.";
	if (damage.extension) {
		ss << " " << damage.exString;
	}
	if (damage.fatal) {
		ss << " (Onslaught)";
	}
	message.type = MESSAGE_DAMAGE_DEALT;
	message.text = ss.str();
}

void Game::sendEffects(
	std::shared_ptr<Creature> target, const CombatDamage &damage, const Position &targetPos, TextMessage &message,
	const CreatureVector &spectators
) {
	uint16_t hitEffect;
	if (message.primary.value) {
		combatGetTypeInfo(damage.primary.type, target, message.primary.color, hitEffect);
		if (hitEffect != CONST_ME_NONE) {
			addMagicEffect(spectators, targetPos, hitEffect);
		}
	}

	if (message.secondary.value) {
		combatGetTypeInfo(damage.secondary.type, target, message.secondary.color, hitEffect);
		if (hitEffect != CONST_ME_NONE) {
			addMagicEffect(spectators, targetPos, hitEffect);
		}
	}
}

void Game::applyCharmRune(
	std::shared_ptr<Monster> targetMonster, std::shared_ptr<Player> attackerPlayer, std::shared_ptr<Creature> target, const int32_t &realDamage
) const {
	if (!targetMonster || !attackerPlayer) {
		return;
	}
	if (charmRune_t activeCharm = g_iobestiary().getCharmFromTarget(attackerPlayer, g_monsters().getMonsterTypeByRaceId(targetMonster->getRaceId()));
		activeCharm != CHARM_NONE) {
		const auto charm = g_iobestiary().getBestiaryCharm(activeCharm);
		int8_t chance = charm->id == CHARM_CRIPPLE ? charm->chance : charm->chance + attackerPlayer->getCharmChanceModifier();
		g_logger().debug("charm chance: {}, base: {}, bonus: {}", chance, charm->chance, attackerPlayer->getCharmChanceModifier());
		if (charm->type == CHARM_OFFENSIVE && (chance >= normal_random(0, 100))) {
			g_iobestiary().parseCharmCombat(charm, attackerPlayer, target, realDamage);
		}
	}
}

// Mana leech
void Game::applyManaLeech(
	std::shared_ptr<Player> attackerPlayer, std::shared_ptr<Monster> targetMonster, std::shared_ptr<Creature> target, const CombatDamage &damage, const int32_t &realDamage
) const {
	// Wheel of destiny bonus - mana leech chance and amount
	auto wheelLeechChance = attackerPlayer->wheel()->checkDrainBodyLeech(target, SKILL_MANA_LEECH_CHANCE);
	auto wheelLeechAmount = attackerPlayer->wheel()->checkDrainBodyLeech(target, SKILL_MANA_LEECH_AMOUNT);

	uint16_t manaChance = attackerPlayer->getSkillLevel(SKILL_MANA_LEECH_CHANCE) + wheelLeechChance + damage.manaLeechChance;
	uint16_t manaSkill = attackerPlayer->getSkillLevel(SKILL_MANA_LEECH_AMOUNT) + wheelLeechAmount + damage.manaLeech;
	if (normal_random(0, 100) >= manaChance) {
		return;
	}
	// Void charm rune
	if (targetMonster) {
		if (uint16_t playerCharmRaceidVoid = attackerPlayer->parseRacebyCharm(CHARM_VOID, false, 0);
			playerCharmRaceidVoid != 0 && playerCharmRaceidVoid == targetMonster->getRace()) {
			if (const auto charm = g_iobestiary().getBestiaryCharm(CHARM_VOID)) {
				manaSkill += charm->percent;
			}
		}
	}
	CombatParams tmpParams;
	CombatDamage tmpDamage;

	int affected = damage.affected;
	tmpDamage.origin = ORIGIN_SPELL;
	tmpDamage.primary.type = COMBAT_MANADRAIN;
	tmpDamage.primary.value = calculateLeechAmount(realDamage, manaSkill, affected);

	Combat::doCombatMana(nullptr, attackerPlayer, tmpDamage, tmpParams);
}

// Life leech
void Game::applyLifeLeech(
	std::shared_ptr<Player> attackerPlayer, std::shared_ptr<Monster> targetMonster, std::shared_ptr<Creature> target, const CombatDamage &damage, const int32_t &realDamage
) const {
	// Wheel of destiny bonus - life leech chance and amount
	auto wheelLeechChance = attackerPlayer->wheel()->checkDrainBodyLeech(target, SKILL_LIFE_LEECH_CHANCE);
	auto wheelLeechAmount = attackerPlayer->wheel()->checkDrainBodyLeech(target, SKILL_LIFE_LEECH_AMOUNT);
	uint16_t lifeChance = attackerPlayer->getSkillLevel(SKILL_LIFE_LEECH_CHANCE) + wheelLeechChance + damage.lifeLeechChance;
	uint16_t lifeSkill = attackerPlayer->getSkillLevel(SKILL_LIFE_LEECH_AMOUNT) + wheelLeechAmount + damage.lifeLeech;
	if (normal_random(0, 100) >= lifeChance) {
		return;
	}
	if (targetMonster) {
		if (uint16_t playerCharmRaceidVamp = attackerPlayer->parseRacebyCharm(CHARM_VAMP, false, 0);
			playerCharmRaceidVamp != 0 && playerCharmRaceidVamp == targetMonster->getRaceId()) {
			if (const auto lifec = g_iobestiary().getBestiaryCharm(CHARM_VAMP)) {
				lifeSkill += lifec->percent;
			}
		}
	}
	CombatParams tmpParams;
	CombatDamage tmpDamage;

	int affected = damage.affected;
	tmpDamage.origin = ORIGIN_SPELL;
	tmpDamage.primary.type = COMBAT_HEALING;
	tmpDamage.primary.value = calculateLeechAmount(realDamage, lifeSkill, affected);

	Combat::doCombatHealth(nullptr, attackerPlayer, tmpDamage, tmpParams);
}

int32_t Game::calculateLeechAmount(const int32_t &realDamage, const uint16_t &skillAmount, int targetsAffected) const {
	auto intermediateResult = realDamage * (skillAmount / 10000.0) * (0.1 * targetsAffected + 0.9) / targetsAffected;
	return std::clamp<int32_t>(static_cast<int32_t>(std::lround(intermediateResult)), 0, realDamage);
}

bool Game::combatChangeMana(std::shared_ptr<Creature> attacker, std::shared_ptr<Creature> target, CombatDamage &damage) {
	const Position &targetPos = target->getPosition();
	auto manaChange = damage.primary.value + damage.secondary.value;
	if (manaChange > 0) {
		std::shared_ptr<Player> attackerPlayer;
		if (attacker) {
			attackerPlayer = attacker->getPlayer();
		} else {
			attackerPlayer = nullptr;
		}

		auto targetPlayer = target->getPlayer();
		if (attackerPlayer && targetPlayer && attackerPlayer->getSkull() == SKULL_BLACK && attackerPlayer->getSkullClient(targetPlayer) == SKULL_NONE) {
			return false;
		}

		if (damage.origin != ORIGIN_NONE) {
			const auto events = target->getCreatureEvents(CREATURE_EVENT_MANACHANGE);
			if (!events.empty()) {
				for (const auto creatureEvent : events) {
					creatureEvent->executeManaChange(target, attacker, damage);
				}
				damage.origin = ORIGIN_NONE;
				return combatChangeMana(attacker, target, damage);
			}
		}

		auto realManaChange = target->getMana();
		target->changeMana(manaChange);
		realManaChange = target->getMana() - realManaChange;

		if (realManaChange > 0 && !target->isInGhostMode()) {
			std::string damageString = fmt::format("{} mana", realManaChange);

			std::string spectatorMessage;
			if (!attacker) {
				spectatorMessage += ucfirst(target->getNameDescription());
				spectatorMessage += " was restored for " + damageString;
			} else {
				spectatorMessage += ucfirst(attacker->getNameDescription());
				spectatorMessage += " restored ";
				if (attacker == target) {
					spectatorMessage += (targetPlayer ? targetPlayer->getReflexivePronoun() : "itself");
				} else {
					spectatorMessage += target->getNameDescription();
				}
				spectatorMessage += " for " + damageString;
			}

			TextMessage message;
			message.position = targetPos;
			message.primary.value = realManaChange;
			message.primary.color = TEXTCOLOR_MAYABLUE;

			for (const auto &spectator : Spectators().find<Player>(targetPos)) {
				const auto &tmpPlayer = spectator->getPlayer();
				if (!tmpPlayer) {
					continue;
				}

				if (tmpPlayer == attackerPlayer && attackerPlayer != targetPlayer) {
					message.type = MESSAGE_HEALED;
					message.text = "You restored " + target->getNameDescription() + " for " + damageString;
				} else if (tmpPlayer == targetPlayer) {
					message.type = MESSAGE_HEALED;
					if (!attacker) {
						message.text = "You were restored for " + damageString;
					} else if (targetPlayer == attackerPlayer) {
						message.text = "You restore yourself for " + damageString;
					} else {
						message.text = "You were restored by " + attacker->getNameDescription() + " for " + damageString;
					}
				} else {
					message.type = MESSAGE_HEALED_OTHERS;
					message.text = spectatorMessage;
				}
				tmpPlayer->sendTextMessage(message);
			}
		}
	} else {
		if (!target->isAttackable()) {
			if (!target->isInGhostMode()) {
				addMagicEffect(targetPos, CONST_ME_POFF);
			}
			return false;
		}

		std::shared_ptr<Player> attackerPlayer;
		if (attacker) {
			attackerPlayer = attacker->getPlayer();
		} else {
			attackerPlayer = nullptr;
		}

		auto targetPlayer = target->getPlayer();
		if (attackerPlayer && targetPlayer && attackerPlayer->getSkull() == SKULL_BLACK && attackerPlayer->getSkullClient(targetPlayer) == SKULL_NONE) {
			return false;
		}

		auto manaLoss = std::min<int32_t>(target->getMana(), -manaChange);
		BlockType_t blockType = target->blockHit(attacker, COMBAT_MANADRAIN, manaLoss);
		if (blockType != BLOCK_NONE) {
			addMagicEffect(targetPos, CONST_ME_POFF);
			return false;
		}

		if (manaLoss <= 0) {
			return true;
		}

		if (damage.origin != ORIGIN_NONE) {
			const auto events = target->getCreatureEvents(CREATURE_EVENT_MANACHANGE);
			if (!events.empty()) {
				for (const auto creatureEvent : events) {
					creatureEvent->executeManaChange(target, attacker, damage);
				}
				damage.origin = ORIGIN_NONE;
				return combatChangeMana(attacker, target, damage);
			}
		}

		if (targetPlayer && attacker && attacker->getMonster()) {
			// Charm rune (target as player)
			const auto mType = g_monsters().getMonsterType(attacker->getName());
			if (mType) {
				charmRune_t activeCharm = g_iobestiary().getCharmFromTarget(targetPlayer, mType);
				if (activeCharm != CHARM_NONE && activeCharm != CHARM_CLEANSE) {
					const auto charm = g_iobestiary().getBestiaryCharm(activeCharm);
					if (charm && charm->type == CHARM_DEFENSIVE && (charm->chance > normal_random(0, 100))) {
						if (g_iobestiary().parseCharmCombat(charm, targetPlayer, attacker, manaChange)) {
							sendDoubleSoundEffect(targetPlayer->getPosition(), charm->soundCastEffect, charm->soundImpactEffect, targetPlayer);
							return false; // Dodge charm
						}
					}
				}
			}
		}

		target->drainMana(attacker, manaLoss);

		std::stringstream ss;

		std::string damageString = std::to_string(manaLoss);

		std::string spectatorMessage;

		TextMessage message;
		message.position = targetPos;
		message.primary.value = manaLoss;
		message.primary.color = TEXTCOLOR_BLUE;

		for (const auto &spectator : Spectators().find<Player>(targetPos)) {
			const auto &tmpPlayer = spectator->getPlayer();
			if (!tmpPlayer) {
				continue;
			}

			if (tmpPlayer == attackerPlayer && attackerPlayer != targetPlayer) {
				ss.str({});
				ss << ucfirst(target->getNameDescription()) << " loses " << damageString << " mana due to your attack.";
				message.type = MESSAGE_DAMAGE_DEALT;
				message.text = ss.str();
			} else if (tmpPlayer == targetPlayer) {
				ss.str({});
				ss << "You lose " << damageString << " mana";
				if (!attacker) {
					ss << '.';
				} else if (targetPlayer == attackerPlayer) {
					ss << " due to your own attack.";
				} else {
					ss << " mana due to an attack by " << attacker->getNameDescription() << '.';
				}
				message.type = MESSAGE_DAMAGE_RECEIVED;
				message.text = ss.str();
			} else {
				if (spectatorMessage.empty()) {
					ss.str({});
					ss << ucfirst(target->getNameDescription()) << " loses " << damageString << " mana";
					if (attacker) {
						ss << " due to ";
						if (attacker == target) {
							ss << (targetPlayer ? targetPlayer->getPossessivePronoun() : "its") << " own attack";
						} else {
							ss << "an attack by " << attacker->getNameDescription();
						}
					}
					ss << '.';
					spectatorMessage = ss.str();
				}
				message.type = MESSAGE_DAMAGE_OTHERS;
				message.text = spectatorMessage;
			}
			tmpPlayer->sendTextMessage(message);
		}
	}

	return true;
}

void Game::addCreatureHealth(std::shared_ptr<Creature> target) {
	auto spectators = Spectators().find<Player>(target->getPosition(), true);
	addCreatureHealth(spectators.data(), target);
}

void Game::addCreatureHealth(const CreatureVector &spectators, std::shared_ptr<Creature> target) {
	uint8_t healthPercent = std::ceil((static_cast<double>(target->getHealth()) / std::max<int32_t>(target->getMaxHealth(), 1)) * 100);
	if (const auto &targetPlayer = target->getPlayer()) {
		if (const auto &party = targetPlayer->getParty()) {
			party->updatePlayerHealth(targetPlayer, target, healthPercent);
		}
	} else if (const auto &master = target->getMaster()) {
		if (const auto &masterPlayer = master->getPlayer()) {
			if (const auto &party = masterPlayer->getParty()) {
				party->updatePlayerHealth(masterPlayer, target, healthPercent);
			}
		}
	}
	for (const auto &spectator : spectators) {
		if (const auto &tmpPlayer = spectator->getPlayer()) {
			tmpPlayer->sendCreatureHealth(target);
		}
	}
}

void Game::addPlayerMana(std::shared_ptr<Player> target) {
	if (const auto &party = target->getParty()) {
		uint8_t manaPercent = std::ceil((static_cast<double>(target->getMana()) / std::max<int32_t>(target->getMaxMana(), 1)) * 100);
		party->updatePlayerMana(target, manaPercent);
	}
}

void Game::addPlayerVocation(std::shared_ptr<Player> target) {
	if (const auto &party = target->getParty()) {
		party->updatePlayerVocation(target);
	}

	for (const auto &spectator : Spectators().find<Player>(target->getPosition(), true)) {
		spectator->getPlayer()->sendPlayerVocation(target);
	}
}

void Game::addMagicEffect(const Position &pos, uint16_t effect) {
	auto spectators = Spectators().find<Player>(pos, true);
	addMagicEffect(spectators.data(), pos, effect);
}

void Game::addMagicEffect(const CreatureVector &spectators, const Position &pos, uint16_t effect) {
	for (const auto &spectator : spectators) {
		if (const auto &tmpPlayer = spectator->getPlayer()) {
			tmpPlayer->sendMagicEffect(pos, effect);
		}
	}
}

void Game::removeMagicEffect(const Position &pos, uint16_t effect) {
	auto spectators = Spectators().find<Player>(pos, true);
	removeMagicEffect(spectators.data(), pos, effect);
}

void Game::removeMagicEffect(const CreatureVector &spectators, const Position &pos, uint16_t effect) {
	for (const auto &spectator : spectators) {
		if (const auto &tmpPlayer = spectator->getPlayer()) {
			tmpPlayer->removeMagicEffect(pos, effect);
		}
	}
}

void Game::addDistanceEffect(const Position &fromPos, const Position &toPos, uint16_t effect) {
	auto spectators = Spectators().find<Player>(fromPos).find<Player>(toPos);
	addDistanceEffect(spectators.data(), fromPos, toPos, effect);
}

void Game::addDistanceEffect(const CreatureVector &spectators, const Position &fromPos, const Position &toPos, uint16_t effect) {
	for (const auto &spectator : spectators) {
		if (const auto &tmpPlayer = spectator->getPlayer()) {
			tmpPlayer->sendDistanceShoot(fromPos, toPos, effect);
		}
	}
}

void Game::checkImbuements() {
	for (const auto &[mapPlayerId, mapPlayer] : getPlayers()) {
		if (!mapPlayer) {
			continue;
		}

		mapPlayer->updateInventoryImbuement();
	}
}

void Game::checkLight() {
	lightHour += lightHourDelta;

	if (lightHour > LIGHT_DAY_LENGTH) {
		lightHour -= LIGHT_DAY_LENGTH;
	}

	if (std::abs(lightHour - SUNRISE) < 2 * lightHourDelta) {
		lightState = LIGHT_STATE_SUNRISE;
	} else if (std::abs(lightHour - SUNSET) < 2 * lightHourDelta) {
		lightState = LIGHT_STATE_SUNSET;
	}

	int32_t newLightLevel = lightLevel;
	bool lightChange = false;

	switch (lightState) {
		case LIGHT_STATE_SUNRISE: {
			newLightLevel += (LIGHT_LEVEL_DAY - LIGHT_LEVEL_NIGHT) / 30;
			lightChange = true;
			break;
		}
		case LIGHT_STATE_SUNSET: {
			newLightLevel -= (LIGHT_LEVEL_DAY - LIGHT_LEVEL_NIGHT) / 30;
			lightChange = true;
			break;
		}
		default:
			break;
	}

	if (newLightLevel <= LIGHT_LEVEL_NIGHT) {
		lightLevel = LIGHT_LEVEL_NIGHT;
		lightState = LIGHT_STATE_NIGHT;
	} else if (newLightLevel >= LIGHT_LEVEL_DAY) {
		lightLevel = LIGHT_LEVEL_DAY;
		lightState = LIGHT_STATE_DAY;
	} else {
		lightLevel = newLightLevel;
	}

	LightInfo lightInfo = getWorldLightInfo();

	if (lightChange) {
		for ([[maybe_unused]] const auto &[mapPlayerId, mapPlayer] : getPlayers()) {
			mapPlayer->sendWorldLight(lightInfo);
			mapPlayer->sendTibiaTime(lightHour);
		}
	} else {
		for ([[maybe_unused]] const auto &[mapPlayerId, mapPlayer] : getPlayers()) {
			mapPlayer->sendTibiaTime(lightHour);
		}
	}
	if (currentLightState != lightState) {
		currentLightState = lightState;
		for (const auto &[eventName, globalEvent] : g_globalEvents().getEventMap(GLOBALEVENT_PERIODCHANGE)) {
			globalEvent->executePeriodChange(lightState, lightInfo);
		}
	}
}

LightInfo Game::getWorldLightInfo() const {
	return { lightLevel, 0xD7 };
}

bool Game::gameIsDay() {
	if (lightHour >= (6 * 60) && lightHour <= (18 * 60)) {
		isDay = true;
	} else {
		isDay = false;
	}

	return isDay;
}

void Game::dieSafely(std::string errorMsg /* = "" */) {
	g_logger().error(errorMsg);
	shutdown();
}

void Game::shutdown() {
	g_webhook().sendMessage(":red_circle: Server is shutting down...");

	g_logger().info("Shutting down...");
	map.spawnsMonster.clear();
	map.spawnsNpc.clear();
	raids.clear();

	cleanup();

	if (serviceManager) {
		serviceManager->stop();
	}

	ConnectionManager::getInstance().closeAll();

	g_luaEnvironment().collectGarbage();

	g_logger().info("Done!");
}

void Game::cleanup() {
	for (auto it = browseFields.begin(); it != browseFields.end();) {
		if (it->second.expired()) {
			it = browseFields.erase(it);
		} else {
			++it;
		}
	}
}

void Game::addBestiaryList(uint16_t raceid, std::string name) {
	auto it = BestiaryList.find(raceid);
	if (it != BestiaryList.end()) {
		return;
	}

	BestiaryList.insert(std::pair<uint16_t, std::string>(raceid, name));
}

void Game::broadcastMessage(const std::string &text, MessageClasses type) const {
	g_logger().info("Broadcasted message: {}", text);
	for (const auto &it : players) {
		it.second->sendTextMessage(type, text);
	}
}

void Game::updateCreatureWalkthrough(std::shared_ptr<Creature> creature) {
	// Send to clients
	for (const auto spectator : Spectators().find<Player>(creature->getPosition(), true)) {
		const auto &tmpPlayer = spectator->getPlayer();
		tmpPlayer->sendCreatureWalkthrough(creature, tmpPlayer->canWalkthroughEx(creature));
	}
}

void Game::updateCreatureSkull(std::shared_ptr<Creature> creature) {
	if (getWorldType() != WORLD_TYPE_PVP) {
		return;
	}

	for (const auto &spectator : Spectators().find<Player>(creature->getPosition(), true)) {
		spectator->getPlayer()->sendCreatureSkull(creature);
	}
}

void Game::updatePlayerShield(std::shared_ptr<Player> player) {
	for (const auto &spectator : Spectators().find<Player>(player->getPosition(), true)) {
		spectator->getPlayer()->sendCreatureShield(player);
	}
}

void Game::updateCreatureType(std::shared_ptr<Creature> creature) {
	if (!creature) {
		return;
	}

	std::shared_ptr<Player> masterPlayer = nullptr;
	CreatureType_t creatureType = creature->getType();
	if (creatureType == CREATURETYPE_MONSTER) {
		std::shared_ptr<Creature> master = creature->getMaster();
		if (master) {
			masterPlayer = master->getPlayer();
			if (masterPlayer) {
				creatureType = CREATURETYPE_SUMMON_OTHERS;
			}
		}
	}
	if (creature->isHealthHidden()) {
		creatureType = CREATURETYPE_HIDDEN;
	}

	// Send to clients
	auto spectators = Spectators().find<Player>(creature->getPosition(), true);
	if (creatureType == CREATURETYPE_SUMMON_OTHERS) {
		for (const auto &spectator : spectators) {
			spectator->getPlayer()->sendCreatureType(creature, masterPlayer == spectator ? CREATURETYPE_SUMMON_PLAYER : creatureType);
		}
	} else {
		for (const auto &spectator : spectators) {
			spectator->getPlayer()->sendCreatureType(creature, creatureType);
		}
	}
}

void Game::loadMotdNum() {
	Database &db = Database::getInstance();

	DBResult_ptr result = db.storeQuery("SELECT `value` FROM `server_config` WHERE `config` = 'motd_num'");
	if (result) {
		motdNum = result->getNumber<uint32_t>("value");
	} else {
		db.executeQuery("INSERT INTO `server_config` (`config`, `value`) VALUES ('motd_num', '0')");
	}

	result = db.storeQuery("SELECT `value` FROM `server_config` WHERE `config` = 'motd_hash'");
	if (result) {
		motdHash = result->getString("value");
		if (motdHash != transformToSHA1(g_configManager().getString(SERVER_MOTD, __FUNCTION__))) {
			++motdNum;
		}
	} else {
		db.executeQuery("INSERT INTO `server_config` (`config`, `value`) VALUES ('motd_hash', '')");
	}
}

void Game::saveMotdNum() const {
	Database &db = Database::getInstance();

	std::ostringstream query;
	query << "UPDATE `server_config` SET `value` = '" << motdNum << "' WHERE `config` = 'motd_num'";
	db.executeQuery(query.str());

	query.str(std::string());
	query << "UPDATE `server_config` SET `value` = '" << transformToSHA1(g_configManager().getString(SERVER_MOTD, __FUNCTION__)) << "' WHERE `config` = 'motd_hash'";
	db.executeQuery(query.str());
}

void Game::checkPlayersRecord() {
	const size_t playersOnline = getPlayersOnline();
	if (playersOnline > playersRecord) {
		uint32_t previousRecord = playersRecord;
		playersRecord = playersOnline;

		for (auto &[key, it] : g_globalEvents().getEventMap(GLOBALEVENT_RECORD)) {
			it->executeRecord(playersRecord, previousRecord);
		}
		updatePlayersRecord();
	}
}

void Game::updatePlayersRecord() const {
	Database &db = Database::getInstance();

	std::ostringstream query;
	query << "UPDATE `server_config` SET `value` = '" << playersRecord << "' WHERE `config` = 'players_record'";
	db.executeQuery(query.str());
}

void Game::loadPlayersRecord() {
	Database &db = Database::getInstance();

	DBResult_ptr result = db.storeQuery("SELECT `value` FROM `server_config` WHERE `config` = 'players_record'");
	if (result) {
		playersRecord = result->getNumber<uint32_t>("value");
	} else {
		db.executeQuery("INSERT INTO `server_config` (`config`, `value`) VALUES ('players_record', '0')");
	}
}

void Game::playerInviteToParty(uint32_t playerId, uint32_t invitedId) {
	// Prevent crafted packets from inviting urself to a party (using OTClient)
	if (playerId == invitedId) {
		return;
	}

	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	std::shared_ptr<Player> invitedPlayer = getPlayerByID(invitedId);
	if (!invitedPlayer || invitedPlayer->isInviting(player)) {
		return;
	}

	if (invitedPlayer->getParty()) {
		std::ostringstream ss;
		ss << invitedPlayer->getName() << " is already in a party.";
		player->sendTextMessage(MESSAGE_PARTY_MANAGEMENT, ss.str());
		return;
	}

	std::shared_ptr<Party> party = player->getParty();
	if (!party) {
		party = Party::create(player);
	} else if (party->getLeader() != player) {
		return;
	}

	party->invitePlayer(invitedPlayer);
}

void Game::updatePlayerHelpers(std::shared_ptr<Player> player) {
	if (!player) {
		return;
	}

	const uint16_t helpers = player->getHelpers();
	for (const auto &spectator : Spectators().find<Player>(player->getPosition(), true)) {
		spectator->getPlayer()->sendCreatureHelpers(player->getID(), helpers);
	}
}

void Game::playerJoinParty(uint32_t playerId, uint32_t leaderId) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	std::shared_ptr<Player> leader = getPlayerByID(leaderId);
	if (!leader || !leader->isInviting(player)) {
		return;
	}

	auto party = leader->getParty();
	if (!party || party->getLeader() != leader) {
		return;
	}

	if (player->getParty()) {
		player->sendTextMessage(MESSAGE_PARTY_MANAGEMENT, "You are already in a party.");
		return;
	}

	party->joinParty(player);
}

void Game::playerRevokePartyInvitation(uint32_t playerId, uint32_t invitedId) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	std::shared_ptr<Party> party = player->getParty();
	if (!party || party->getLeader() != player) {
		return;
	}

	std::shared_ptr<Player> invitedPlayer = getPlayerByID(invitedId);
	if (!invitedPlayer || !player->isInviting(invitedPlayer)) {
		return;
	}

	party->revokeInvitation(invitedPlayer);
}

void Game::playerPassPartyLeadership(uint32_t playerId, uint32_t newLeaderId) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	std::shared_ptr<Party> party = player->getParty();
	if (!party || party->getLeader() != player) {
		return;
	}

	std::shared_ptr<Player> newLeader = getPlayerByID(newLeaderId);
	if (!newLeader || !player->isPartner(newLeader)) {
		return;
	}

	party->passPartyLeadership(newLeader);
}

void Game::playerLeaveParty(uint32_t playerId) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	std::shared_ptr<Party> party = player->getParty();
	if (!party || player->hasCondition(CONDITION_INFIGHT) && !player->getZoneType() == ZONE_PROTECTION) {
		player->sendTextMessage(TextMessage(MESSAGE_FAILURE, "You cannot leave party, contact the administrator."));
		return;
	}

	party->leaveParty(player);
}

void Game::playerEnableSharedPartyExperience(uint32_t playerId, bool sharedExpActive) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	auto party = player->getParty();
	auto playerTile = player->getTile();
	if (!party || (player->hasCondition(CONDITION_INFIGHT) && playerTile && !playerTile->hasFlag(TILESTATE_PROTECTIONZONE))) {
		return;
	}

	party->setSharedExperience(player, sharedExpActive);
}

void Game::sendGuildMotd(uint32_t playerId) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	const auto guild = player->getGuild();
	if (guild) {
		player->sendChannelMessage("Message of the Day", guild->getMotd(), TALKTYPE_CHANNEL_R1, CHANNEL_GUILD);
	}
}

void Game::kickPlayer(uint32_t playerId, bool displayEffect) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	player->removePlayer(displayEffect);
}

void Game::playerCyclopediaCharacterInfo(std::shared_ptr<Player> player, uint32_t characterID, CyclopediaCharacterInfoType_t characterInfoType, uint16_t entriesPerPage, uint16_t page) {
	uint32_t playerGUID = player->getGUID();
	if (characterID != playerGUID) {
		// For now allow viewing only our character since we don't have tournaments supported
		player->sendCyclopediaCharacterNoData(characterInfoType, 2);
		return;
	}

	switch (characterInfoType) {
		case CYCLOPEDIA_CHARACTERINFO_BASEINFORMATION:
			player->sendCyclopediaCharacterBaseInformation();
			break;
		case CYCLOPEDIA_CHARACTERINFO_GENERALSTATS:
			player->sendCyclopediaCharacterGeneralStats();
			break;
		case CYCLOPEDIA_CHARACTERINFO_COMBATSTATS:
			player->sendCyclopediaCharacterCombatStats();
			break;
		case CYCLOPEDIA_CHARACTERINFO_RECENTDEATHS: {
			std::ostringstream query;
			uint32_t offset = static_cast<uint32_t>(page - 1) * entriesPerPage;
			query << "SELECT `time`, `level`, `killed_by`, `mostdamage_by`, (select count(*) FROM `player_deaths` WHERE `player_id` = " << playerGUID << ") as `entries` FROM `player_deaths` WHERE `player_id` = " << playerGUID << " ORDER BY `time` DESC LIMIT " << offset << ", " << entriesPerPage;

			uint32_t playerID = player->getID();
			std::function<void(DBResult_ptr, bool)> callback = [playerID, page, entriesPerPage](DBResult_ptr result, bool) {
				std::shared_ptr<Player> player = g_game().getPlayerByID(playerID);
				if (!player) {
					return;
				}

				player->resetAsyncOngoingTask(PlayerAsyncTask_RecentDeaths);
				if (!result) {
					player->sendCyclopediaCharacterRecentDeaths(0, 0, {});
					return;
				}

				uint32_t pages = result->getNumber<uint32_t>("entries");
				pages += entriesPerPage - 1;
				pages /= entriesPerPage;

				std::vector<RecentDeathEntry> entries;
				entries.reserve(result->countResults());
				do {
					std::string cause1 = result->getString("killed_by");
					std::string cause2 = result->getString("mostdamage_by");

					std::ostringstream cause;
					cause << "Died at Level " << result->getNumber<uint32_t>("level") << " by";
					if (!cause1.empty()) {
						const char &character = cause1.front();
						if (character == 'a' || character == 'e' || character == 'i' || character == 'o' || character == 'u') {
							cause << " an ";
						} else {
							cause << " a ";
						}
						cause << cause1;
					}

					if (!cause2.empty()) {
						if (!cause1.empty()) {
							cause << " and ";
						}

						const char &character = cause2.front();
						if (character == 'a' || character == 'e' || character == 'i' || character == 'o' || character == 'u') {
							cause << " an ";
						} else {
							cause << " a ";
						}
						cause << cause2;
					}
					cause << '.';
					entries.emplace_back(std::move(cause.str()), result->getNumber<uint32_t>("time"));
				} while (result->next());
				player->sendCyclopediaCharacterRecentDeaths(page, static_cast<uint16_t>(pages), entries);
			};
			g_databaseTasks().store(query.str(), callback);
			player->addAsyncOngoingTask(PlayerAsyncTask_RecentDeaths);
			break;
		}
		case CYCLOPEDIA_CHARACTERINFO_RECENTPVPKILLS: {
			// TODO: add guildwar, assists and arena kills
			Database &db = Database::getInstance();
			const std::string &escapedName = db.escapeString(player->getName());
			std::ostringstream query;
			uint32_t offset = static_cast<uint32_t>(page - 1) * entriesPerPage;
			query << "SELECT `d`.`time`, `d`.`killed_by`, `d`.`mostdamage_by`, `d`.`unjustified`, `d`.`mostdamage_unjustified`, `p`.`name`, (select count(*) FROM `player_deaths` WHERE ((`killed_by` = " << escapedName << " AND `is_player` = 1) OR (`mostdamage_by` = " << escapedName << " AND `mostdamage_is_player` = 1))) as `entries` FROM `player_deaths` AS `d` INNER JOIN `players` AS `p` ON `d`.`player_id` = `p`.`id` WHERE ((`d`.`killed_by` = " << escapedName << " AND `d`.`is_player` = 1) OR (`d`.`mostdamage_by` = " << escapedName << " AND `d`.`mostdamage_is_player` = 1)) ORDER BY `time` DESC LIMIT " << offset << ", " << entriesPerPage;

			uint32_t playerID = player->getID();
			std::function<void(DBResult_ptr, bool)> callback = [playerID, page, entriesPerPage](DBResult_ptr result, bool) {
				std::shared_ptr<Player> player = g_game().getPlayerByID(playerID);
				if (!player) {
					return;
				}

				player->resetAsyncOngoingTask(PlayerAsyncTask_RecentPvPKills);
				if (!result) {
					player->sendCyclopediaCharacterRecentPvPKills(0, 0, {});
					return;
				}

				uint32_t pages = result->getNumber<uint32_t>("entries");
				pages += entriesPerPage - 1;
				pages /= entriesPerPage;

				std::vector<RecentPvPKillEntry> entries;
				entries.reserve(result->countResults());
				do {
					std::string cause1 = result->getString("killed_by");
					std::string cause2 = result->getString("mostdamage_by");
					std::string name = result->getString("name");

					uint8_t status = CYCLOPEDIA_CHARACTERINFO_RECENTKILLSTATUS_JUSTIFIED;
					if (player->getName() == cause1) {
						if (result->getNumber<uint32_t>("unjustified") == 1) {
							status = CYCLOPEDIA_CHARACTERINFO_RECENTKILLSTATUS_UNJUSTIFIED;
						}
					} else if (player->getName() == cause2) {
						if (result->getNumber<uint32_t>("mostdamage_unjustified") == 1) {
							status = CYCLOPEDIA_CHARACTERINFO_RECENTKILLSTATUS_UNJUSTIFIED;
						}
					}

					std::ostringstream description;
					description << "Killed " << name << '.';
					entries.emplace_back(std::move(description.str()), result->getNumber<uint32_t>("time"), status);
				} while (result->next());
				player->sendCyclopediaCharacterRecentPvPKills(page, static_cast<uint16_t>(pages), entries);
			};
			g_databaseTasks().store(query.str(), callback);
			player->addAsyncOngoingTask(PlayerAsyncTask_RecentPvPKills);
			break;
		}
		case CYCLOPEDIA_CHARACTERINFO_ACHIEVEMENTS:
			player->sendCyclopediaCharacterAchievements();
			break;
		case CYCLOPEDIA_CHARACTERINFO_ITEMSUMMARY:
			player->sendCyclopediaCharacterItemSummary();
			break;
		case CYCLOPEDIA_CHARACTERINFO_OUTFITSMOUNTS:
			player->sendCyclopediaCharacterOutfitsMounts();
			break;
		case CYCLOPEDIA_CHARACTERINFO_STORESUMMARY:
			player->sendCyclopediaCharacterStoreSummary();
			break;
		case CYCLOPEDIA_CHARACTERINFO_INSPECTION:
			player->sendCyclopediaCharacterInspection();
			break;
		case CYCLOPEDIA_CHARACTERINFO_BADGES:
			player->sendCyclopediaCharacterBadges();
			break;
		case CYCLOPEDIA_CHARACTERINFO_TITLES:
			player->sendCyclopediaCharacterTitles();
			break;
		default:
			player->sendCyclopediaCharacterNoData(characterInfoType, 1);
			break;
	}
}

std::string Game::generateHighscoreQueryForEntries(const std::string &categoryName, uint32_t page, uint8_t entriesPerPage, uint32_t vocation) {
	std::ostringstream query;
	uint32_t startPage = (static_cast<uint32_t>(page - 1) * static_cast<uint32_t>(entriesPerPage));
	uint32_t endPage = startPage + static_cast<uint32_t>(entriesPerPage);

	query << "SELECT *, @row AS `entries`, " << page << " AS `page` FROM (SELECT *, (@row := @row + 1) AS `rn` FROM (SELECT `id`, `name`, `level`, `vocation`, `"
		  << categoryName << "` AS `points`, @curRank := IF(@prevRank = `" << categoryName << "`, @curRank, IF(@prevRank := `" << categoryName
		  << "`, @curRank + 1, @curRank + 1)) AS `rank` FROM `players` `p`, (SELECT @curRank := 0, @prevRank := NULL, @row := 0) `r` WHERE `group_id` < "
		  << static_cast<int>(account::GROUP_TYPE_GAMEMASTER) << " ORDER BY `" << categoryName << "` DESC) `t`";

	if (vocation != 0xFFFFFFFF) {
		query << generateVocationConditionHighscore(vocation);
	}
	query << ") `T` WHERE `rn` > " << startPage << " AND `rn` <= " << endPage;

	return query.str();
}

std::string Game::generateHighscoreQueryForOurRank(const std::string &categoryName, uint8_t entriesPerPage, uint32_t playerGUID, uint32_t vocation) {
	std::ostringstream query;
	std::string entriesStr = std::to_string(entriesPerPage);

	query << "SELECT *, @row AS `entries`, (@ourRow DIV " << entriesStr << ") + 1 AS `page` FROM (SELECT *, (@row := @row + 1) AS `rn`, @ourRow := IF(`id` = "
		  << playerGUID << ", @row - 1, @ourRow) AS `rw` FROM (SELECT `id`, `name`, `level`, `vocation`, `" << categoryName << "` AS `points`, @curRank := IF(@prevRank = `"
		  << categoryName << "`, @curRank, IF(@prevRank := `" << categoryName << "`, @curRank + 1, @curRank + 1)) AS `rank` FROM `players` `p`, (SELECT @curRank := 0, @prevRank := NULL, @row := 0, @ourRow := 0) `r` WHERE `group_id` < "
		  << static_cast<int>(account::GROUP_TYPE_GAMEMASTER) << " ORDER BY `" << categoryName << "` DESC) `t`";

	if (vocation != 0xFFFFFFFF) {
		query << generateVocationConditionHighscore(vocation);
	}
	query << ") `T` WHERE `rn` > ((@ourRow DIV " << entriesStr << ") * " << entriesStr << ") AND `rn` <= (((@ourRow DIV " << entriesStr << ") * " << entriesStr << ") + " << entriesStr << ")";

	return query.str();
}

std::string Game::generateVocationConditionHighscore(uint32_t vocation) {
	std::ostringstream queryPart;
	bool firstVocation = true;

	const auto vocationsMap = g_vocations().getVocations();
	for (const auto &it : vocationsMap) {
		const auto &voc = it.second;
		if (voc.getFromVocation() == vocation) {
			if (firstVocation) {
				queryPart << " WHERE `vocation` = " << voc.getId();
				firstVocation = false;
			} else {
				queryPart << " OR `vocation` = " << voc.getId();
			}
		}
	}

	return queryPart.str();
}

void Game::processHighscoreResults(DBResult_ptr result, uint32_t playerID, uint8_t category, uint32_t vocation, uint8_t entriesPerPage) {
	std::shared_ptr<Player> player = g_game().getPlayerByID(playerID);
	if (!player) {
		return;
	}

	player->resetAsyncOngoingTask(PlayerAsyncTask_Highscore);

	if (!result) {
		player->sendHighscoresNoData();
		return;
	}

	uint16_t page = result->getNumber<uint16_t>("page");
	uint32_t pages = result->getNumber<uint32_t>("entries");
	pages += entriesPerPage - 1;
	pages /= entriesPerPage;

	std::ostringstream cacheKeyStream;
	cacheKeyStream << "Highscore_" << static_cast<int>(category) << "_" << static_cast<int>(vocation) << "_" << static_cast<int>(entriesPerPage) << "_" << page;
	std::string cacheKey = cacheKeyStream.str();

	auto it = highscoreCache.find(cacheKey);
	auto now = std::chrono::system_clock::now();
	if (it != highscoreCache.end() && (now - it->second.timestamp < HIGHSCORE_CACHE_EXPIRATION_TIME)) {
		auto &cacheEntry = it->second;
		auto cachedTime = it->second.timestamp;
		auto durationSinceEpoch = cachedTime.time_since_epoch();
		auto secondsSinceEpoch = std::chrono::duration_cast<std::chrono::seconds>(durationSinceEpoch).count();
		auto updateTimer = static_cast<uint32_t>(secondsSinceEpoch);
		player->sendHighscores(cacheEntry.characters, category, vocation, cacheEntry.page, static_cast<uint16_t>(cacheEntry.entriesPerPage), updateTimer);
	} else {
		std::vector<HighscoreCharacter> characters;
		characters.reserve(result->countResults());
		if (result) {
			do {
				uint8_t characterVocation;
				const auto &voc = g_vocations().getVocation(result->getNumber<uint16_t>("vocation"));
				if (voc) {
					characterVocation = voc->getClientId();
				} else {
					characterVocation = 0;
				}
				characters.emplace_back(std::move(result->getString("name")), result->getNumber<uint64_t>("points"), result->getNumber<uint32_t>("id"), result->getNumber<uint32_t>("rank"), result->getNumber<uint16_t>("level"), characterVocation);
			} while (result->next());
		}

		player->sendHighscores(characters, category, vocation, page, static_cast<uint16_t>(pages), getTimeNow());
		highscoreCache[cacheKey] = { characters, page, pages, now };
	}
}

void Game::cacheQueryHighscore(const std::string &key, const std::string &query, uint32_t page, uint8_t entriesPerPage) {
	QueryHighscoreCacheEntry queryEntry { query, page, entriesPerPage, std::chrono::steady_clock::now() };
	queryCache[key] = queryEntry;
}

std::string Game::generateHighscoreOrGetCachedQueryForEntries(const std::string &categoryName, uint32_t page, uint8_t entriesPerPage, uint32_t vocation) {
	std::ostringstream cacheKeyStream;
	cacheKeyStream << "Entries_" << categoryName << "_" << page << "_" << static_cast<int>(entriesPerPage) << "_" << vocation;
	std::string cacheKey = cacheKeyStream.str();

	if (queryCache.find(cacheKey) != queryCache.end()) {
		const QueryHighscoreCacheEntry &cachedEntry = queryCache[cacheKey];
		if (cachedEntry.page == page) {
			return cachedEntry.query;
		}
	}

	std::string newQuery = generateHighscoreQueryForEntries(categoryName, page, entriesPerPage, vocation);
	cacheQueryHighscore(cacheKey, newQuery, page, entriesPerPage);

	return newQuery;
}

std::string Game::generateHighscoreOrGetCachedQueryForOurRank(const std::string &categoryName, uint8_t entriesPerPage, uint32_t playerGUID, uint32_t vocation) {
	std::ostringstream cacheKeyStream;
	cacheKeyStream << "OurRank_" << categoryName << "_" << static_cast<int>(entriesPerPage) << "_" << playerGUID << "_" << vocation;
	std::string cacheKey = cacheKeyStream.str();

	if (queryCache.find(cacheKey) != queryCache.end()) {
		const QueryHighscoreCacheEntry &cachedEntry = queryCache[cacheKey];
		if (cachedEntry.page == entriesPerPage) {
			return cachedEntry.query;
		}
	}

	std::string newQuery = generateHighscoreQueryForOurRank(categoryName, entriesPerPage, playerGUID, vocation);
	cacheQueryHighscore(cacheKey, newQuery, entriesPerPage, entriesPerPage);

	return newQuery;
}

void Game::playerHighscores(std::shared_ptr<Player> player, HighscoreType_t type, uint8_t category, uint32_t vocation, const std::string &, uint16_t page, uint8_t entriesPerPage) {
	if (player->hasAsyncOngoingTask(PlayerAsyncTask_Highscore)) {
		return;
	}

	std::string categoryName;
	switch (category) {
		case HIGHSCORE_CATEGORY_FIST_FIGHTING:
			categoryName = "skill_fist";
			break;
		case HIGHSCORE_CATEGORY_CLUB_FIGHTING:
			categoryName = "skill_club";
			break;
		case HIGHSCORE_CATEGORY_SWORD_FIGHTING:
			categoryName = "skill_sword";
			break;
		case HIGHSCORE_CATEGORY_AXE_FIGHTING:
			categoryName = "skill_axe";
			break;
		case HIGHSCORE_CATEGORY_DISTANCE_FIGHTING:
			categoryName = "skill_dist";
			break;
		case HIGHSCORE_CATEGORY_SHIELDING:
			categoryName = "skill_shielding";
			break;
		case HIGHSCORE_CATEGORY_FISHING:
			categoryName = "skill_fishing";
			break;
		case HIGHSCORE_CATEGORY_MAGIC_LEVEL:
			categoryName = "maglevel";
			break;
		default: {
			category = HIGHSCORE_CATEGORY_EXPERIENCE;
			categoryName = "experience";
			break;
		}
	}

	std::string query;
	if (type == HIGHSCORE_GETENTRIES) {
		query = generateHighscoreOrGetCachedQueryForEntries(categoryName, page, entriesPerPage, vocation);
	} else if (type == HIGHSCORE_OURRANK) {
		query = generateHighscoreOrGetCachedQueryForOurRank(categoryName, entriesPerPage, player->getGUID(), vocation);
	}

	uint32_t playerID = player->getID();
	std::function<void(DBResult_ptr, bool)> callback = [this, playerID, category, vocation, entriesPerPage](DBResult_ptr result, bool) {
		processHighscoreResults(std::move(result), playerID, category, vocation, entriesPerPage);
	};

	g_databaseTasks().store(query, callback);
	player->addAsyncOngoingTask(PlayerAsyncTask_Highscore);
}

void Game::playerReportRuleViolationReport(uint32_t playerId, const std::string &targetName, uint8_t reportType, uint8_t reportReason, const std::string &comment, const std::string &translation) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	g_events().eventPlayerOnReportRuleViolation(player, targetName, reportType, reportReason, comment, translation);
	g_callbacks().executeCallback(EventCallback_t::playerOnReportRuleViolation, &EventCallback::playerOnReportRuleViolation, player, targetName, reportType, reportReason, comment, translation);
}

void Game::playerReportBug(uint32_t playerId, const std::string &message, const Position &position, uint8_t category) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	g_events().eventPlayerOnReportBug(player, message, position, category);
	g_callbacks().executeCallback(EventCallback_t::playerOnReportBug, &EventCallback::playerOnReportBug, player, message, position, category);
}

void Game::playerDebugAssert(uint32_t playerId, const std::string &assertLine, const std::string &date, const std::string &description, const std::string &comment) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	// TODO: move debug assertions to database
	FILE* file = fopen("client_assertions.txt", "a");
	if (file) {
		fprintf(file, "----- %s - %s (%s) -----\n", formatDate(time(nullptr)).c_str(), player->getName().c_str(), convertIPToString(player->getIP()).c_str());
		fprintf(file, "%s\n%s\n%s\n%s\n", assertLine.c_str(), date.c_str(), description.c_str(), comment.c_str());
		fclose(file);
	}
}

void Game::playerPreyAction(uint32_t playerId, uint8_t slot, uint8_t action, uint8_t option, int8_t index, uint16_t raceId) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	g_ioprey().parsePreyAction(player, static_cast<PreySlot_t>(slot), static_cast<PreyAction_t>(action), static_cast<PreyOption_t>(option), index, raceId);
}

void Game::playerTaskHuntingAction(uint32_t playerId, uint8_t slot, uint8_t action, bool upgrade, uint16_t raceId) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	g_ioprey().parseTaskHuntingAction(player, static_cast<PreySlot_t>(slot), static_cast<PreyTaskAction_t>(action), upgrade, raceId);
}

void Game::playerNpcGreet(uint32_t playerId, uint32_t npcId) {
	const auto &player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	const auto &npc = getNpcByID(npcId);
	if (!npc) {
		return;
	}

	auto spectators = Spectators().find<Player>(player->getPosition(), true);
	spectators.insert(npc);
	internalCreatureSay(player, TALKTYPE_SAY, "hi", false, &spectators);

	auto npcsSpectators = spectators.filter<Npc>();

	if (npc->getSpeechBubble() == SPEECHBUBBLE_TRADE) {
		internalCreatureSay(player, TALKTYPE_PRIVATE_PN, "trade", false, &npcsSpectators);
	} else {
		internalCreatureSay(player, TALKTYPE_PRIVATE_PN, "sail", false, &npcsSpectators);
	}
}

void Game::playerLeaveMarket(uint32_t playerId) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	player->setInMarket(false);
}

void Game::playerBrowseMarket(uint32_t playerId, uint16_t itemId, uint8_t tier) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	if (!player->isInMarket()) {
		return;
	}

	const ItemType &it = Item::items[itemId];
	if (it.id == 0) {
		return;
	}

	if (it.wareId == 0) {
		return;
	}

	const MarketOfferList &buyOffers = IOMarket::getActiveOffers(MARKETACTION_BUY, it.id, tier);
	const MarketOfferList &sellOffers = IOMarket::getActiveOffers(MARKETACTION_SELL, it.id, tier);
	player->sendMarketBrowseItem(it.id, buyOffers, sellOffers, tier);
	player->sendMarketDetail(it.id, tier);
}

void Game::playerBrowseMarketOwnOffers(uint32_t playerId) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	if (!player->isInMarket()) {
		return;
	}

	const MarketOfferList &buyOffers = IOMarket::getOwnOffers(MARKETACTION_BUY, player->getGUID());
	const MarketOfferList &sellOffers = IOMarket::getOwnOffers(MARKETACTION_SELL, player->getGUID());
	player->sendMarketBrowseOwnOffers(buyOffers, sellOffers);
}

void Game::playerBrowseMarketOwnHistory(uint32_t playerId) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	if (!player->isInMarket()) {
		return;
	}

	const HistoryMarketOfferList &buyOffers = IOMarket::getOwnHistory(MARKETACTION_BUY, player->getGUID());
	const HistoryMarketOfferList &sellOffers = IOMarket::getOwnHistory(MARKETACTION_SELL, player->getGUID());
	player->sendMarketBrowseOwnHistory(buyOffers, sellOffers);
}

namespace {
	bool removeOfferItems(const std::shared_ptr<Player> &player, const std::shared_ptr<DepotLocker> &depotLocker, const ItemType &itemType, uint16_t amount, uint8_t tier, std::ostringstream &offerStatus) {
		uint16_t removeAmount = amount;
		if (
			// Init-statement
			auto stashItemCount = player->getStashItemCount(itemType.wareId);
			// Condition
			stashItemCount > 0
		) {
			if (removeAmount > stashItemCount && player->withdrawItem(itemType.wareId, stashItemCount)) {
				removeAmount -= stashItemCount;
			} else if (player->withdrawItem(itemType.wareId, removeAmount)) {
				removeAmount = 0;
			} else {
				offerStatus << "Failed to remove stash items from player " << player->getName();
				return false;
			}
		}

		auto [itemVector, totalCount] = player->getLockerItemsAndCountById(depotLocker, tier, itemType.id);
		if (removeAmount > 0) {
			if (totalCount == 0 || itemVector.size() == 0) {
				offerStatus << "Player " << player->getName() << " not have item for create offer";
				return false;
			}

			uint32_t count = 0;
			for (auto item : itemVector) {
				if (!item) {
					continue;
				}

				if (itemType.stackable) {
					uint16_t removeCount = std::min<uint16_t>(removeAmount, item->getItemCount());
					removeAmount -= removeCount;
					if (
						// Init-statement
						auto ret = g_game().internalRemoveItem(item, removeCount);
						// Condition
						ret != RETURNVALUE_NOERROR
					) {
						offerStatus << "Failed to remove items from player " << player->getName() << " error: " << getReturnMessage(ret);
						return false;
					}

					if (removeAmount == 0) {
						break;
					}
				} else {
					count += Item::countByType(item, -1);
					if (count > amount) {
						break;
					}
					auto ret = g_game().internalRemoveItem(item);
					if (ret != RETURNVALUE_NOERROR) {
						offerStatus << "Failed to remove items from player " << player->getName() << " error: " << getReturnMessage(ret);
						return false;
					} else {
						removeAmount -= 1;
					}
				}
			}
		}
		if (removeAmount > 0) {
			g_logger().error("Player {} tried to sell an item {} without this item", itemType.id, player->getName());
			offerStatus << "The item you tried to market is not correct. Check the item again.";
			return false;
		}
		return true;
	}
} // namespace

bool checkCanInitCreateMarketOffer(std::shared_ptr<Player> player, uint8_t type, const ItemType &it, uint16_t amount, uint64_t price, std::ostringstream &offerStatus) {
	if (!player) {
		offerStatus << "Failed to load player";
		return false;
	}

	if (!player->getAccount()) {
		offerStatus << "Failed to load player account";
		return false;
	}

	if (!player->isInMarket()) {
		offerStatus << "Failed to load market for player " << player->getName();
		return false;
	}

	if (price == 0) {
		offerStatus << "Failed to process price for player " << player->getName();
		return false;
	}

	if (price > 999999999999) {
		offerStatus << "Player " << player->getName() << " is trying to sell an item with a higher than allowed value";
		return false;
	}

	if (type != MARKETACTION_BUY && type != MARKETACTION_SELL) {
		offerStatus << "Failed to process type " << type << "for player " << player->getName();
		return false;
	}

	if (player->isUIExhausted(1000)) {
		player->sendCancelMessage(RETURNVALUE_YOUAREEXHAUSTED);
		return false;
	}

	if (it.id == 0 || it.wareId == 0) {
		offerStatus << "Failed to load offer or item id";
		return false;
	}

	if (amount == 0 || !it.stackable && amount > 2000 || it.stackable && amount > 64000) {
		offerStatus << "Failed to load amount " << amount << " for player " << player->getName();
		return false;
	}

	g_logger().debug("{} - Offer amount: {}", __FUNCTION__, amount);

	if (g_configManager().getBoolean(MARKET_PREMIUM, __FUNCTION__) && !player->isPremium()) {
		player->sendTextMessage(MESSAGE_MARKET, "Only premium accounts may create offers for that object.");
		return false;
	}

	const uint32_t maxOfferCount = g_configManager().getNumber(MAX_MARKET_OFFERS_AT_A_TIME_PER_PLAYER, __FUNCTION__);
	if (maxOfferCount != 0 && IOMarket::getPlayerOfferCount(player->getGUID()) >= maxOfferCount) {
		offerStatus << "Player " << player->getName() << "excedeed max offer count " << maxOfferCount;
		return false;
	}

	return true;
}

void Game::playerCreateMarketOffer(uint32_t playerId, uint8_t type, uint16_t itemId, uint16_t amount, uint64_t price, uint8_t tier, bool anonymous) {
	// Initialize variables
	// Before creating the offer we will compare it with the RETURN VALUE ERROR
	std::ostringstream offerStatus;
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	const ItemType &it = Item::items[itemId];

	// Make sure everything is ok before the create market offer starts
	if (!checkCanInitCreateMarketOffer(player, type, it, amount, price, offerStatus)) {
		g_logger().error("{} - Player {} had an error on init offer on the market, error code: {}", __FUNCTION__, player->getName(), offerStatus.str());
		return;
	}

	uint64_t calcFee = (price / 100) * amount;
	uint64_t minFee = std::min<uint64_t>(100000, calcFee);
	uint64_t fee = std::max<uint64_t>(20, minFee);

	if (type == MARKETACTION_SELL) {
		if (fee > (player->getBankBalance() + player->getMoney())) {
			offerStatus << "Fee is greater than player money";
			return;
		}

		std::shared_ptr<DepotLocker> depotLocker = player->getDepotLocker(player->getLastDepotId());
		if (depotLocker == nullptr) {
			offerStatus << "Depot locker is nullptr for player " << player->getName();
			return;
		}

		if (it.id == ITEM_STORE_COIN) {
			auto [transferableCoins, result] = player->getAccount()->getCoins(account::CoinType::TRANSFERABLE);

			if (amount > transferableCoins) {
				offerStatus << "Amount is greater than coins for player " << player->getName();
				return;
			}

			// Do not register a transaction for coins creating an offer
			player->getAccount()->removeCoins(account::CoinType::TRANSFERABLE, static_cast<uint32_t>(amount), "");
		} else {
			if (!removeOfferItems(player, depotLocker, it, amount, tier, offerStatus)) {
				g_logger().error("[{}] failed to remove item with id {}, from player {}, errorcode: {}", __FUNCTION__, it.id, player->getName(), offerStatus.str());
				return;
			}
		}

		g_game().removeMoney(player, fee, 0, true);
		g_metrics().addCounter("balance_decrease", fee, { { "player", player->getName() }, { "context", "market_fee" } });
	} else {
		uint64_t totalPrice = price * amount;
		totalPrice += fee;
		if (totalPrice > (player->getMoney() + player->getBankBalance())) {
			offerStatus << "Fee is greater than player money (buy offer)";
			return;
		}

		g_game().removeMoney(player, totalPrice, 0, true);
		g_metrics().addCounter("balance_decrease", totalPrice, { { "player", player->getName() }, { "context", "market_offer" } });
	}

	// Send market window again for update item stats and avoid item clone
	player->sendMarketEnter(player->getLastDepotId());

	// If there is any error, then we will send the log and block the creation of the offer to avoid clone of items
	// The player may lose the item as it will have already been removed, but will not clone
	if (!offerStatus.str().empty()) {
		if (offerStatus.str() == "The item you tried to market is not correct. Check the item again.") {
			player->sendTextMessage(MESSAGE_MARKET, offerStatus.str());
		} else {
			player->sendTextMessage(MESSAGE_MARKET, "There was an error processing your offer, please contact the administrator.");
		}
		g_logger().error("{} - Player {} had an error creating an offer on the market, error code: {}", __FUNCTION__, player->getName(), offerStatus.str());
		return;
	}

	IOMarket::createOffer(player->getGUID(), static_cast<MarketAction_t>(type), it.id, amount, price, tier, anonymous);

	const MarketOfferList &buyOffers = IOMarket::getActiveOffers(MARKETACTION_BUY, it.id, tier);
	const MarketOfferList &sellOffers = IOMarket::getActiveOffers(MARKETACTION_SELL, it.id, tier);
	player->sendMarketBrowseItem(it.id, buyOffers, sellOffers, tier);

	// Exhausted for create offert in the market
	player->updateUIExhausted();
	g_saveManager().savePlayer(player);
}

void Game::playerCancelMarketOffer(uint32_t playerId, uint32_t timestamp, uint16_t counter) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player || !player->getAccount()) {
		return;
	}

	if (!player->isInMarket()) {
		return;
	}

	if (player->isUIExhausted(1000)) {
		player->sendCancelMessage(RETURNVALUE_YOUAREEXHAUSTED);
		return;
	}

	MarketOfferEx offer = IOMarket::getOfferByCounter(timestamp, counter);
	if (offer.id == 0 || offer.playerId != player->getGUID()) {
		return;
	}

	if (offer.type == MARKETACTION_BUY) {
		player->setBankBalance(player->getBankBalance() + offer.price * offer.amount);
		g_metrics().addCounter("balance_decrease", offer.price * offer.amount, { { "player", player->getName() }, { "context", "market_purchase" } });
		// Send market window again for update stats
		player->sendMarketEnter(player->getLastDepotId());
	} else {
		const ItemType &it = Item::items[offer.itemId];
		if (it.id == 0) {
			return;
		}

		if (it.id == ITEM_STORE_COIN) {
			// Do not register a transaction for coins upon cancellation
			player->getAccount()->addCoins(account::CoinType::TRANSFERABLE, offer.amount, "");
		} else if (it.stackable) {
			uint16_t tmpAmount = offer.amount;
			while (tmpAmount > 0) {
				int32_t stackCount = std::min<int32_t>(it.stackSize, tmpAmount);
				std::shared_ptr<Item> item = Item::CreateItem(it.id, stackCount);
				if (internalAddItem(player->getInbox(), item, INDEX_WHEREEVER, FLAG_NOLIMIT) != RETURNVALUE_NOERROR) {
					break;
				}

				if (offer.tier > 0) {
					item->setAttribute(ItemAttribute_t::TIER, offer.tier);
				}

				tmpAmount -= stackCount;
			}
		} else {
			int32_t subType;
			if (it.charges != 0) {
				subType = it.charges;
			} else {
				subType = -1;
			}

			for (uint16_t i = 0; i < offer.amount; ++i) {
				std::shared_ptr<Item> item = Item::CreateItem(it.id, subType);
				if (internalAddItem(player->getInbox(), item, INDEX_WHEREEVER, FLAG_NOLIMIT) != RETURNVALUE_NOERROR) {
					break;
				}

				if (offer.tier > 0) {
					item->setAttribute(ItemAttribute_t::TIER, offer.tier);
				}
			}
		}
	}

	IOMarket::moveOfferToHistory(offer.id, OFFERSTATE_CANCELLED);

	offer.amount = 0;
	offer.timestamp += g_configManager().getNumber(MARKET_OFFER_DURATION, __FUNCTION__);
	player->sendMarketCancelOffer(offer);
	// Send market window again for update stats
	player->sendMarketEnter(player->getLastDepotId());
	// Exhausted for cancel offer in the market
	player->updateUIExhausted();
	g_saveManager().savePlayer(player);
}

void Game::playerAcceptMarketOffer(uint32_t playerId, uint32_t timestamp, uint16_t counter, uint16_t amount) {
	std::ostringstream offerStatus;
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player || !player->getAccount()) {
		offerStatus << "Failed to load player";
		return;
	}

	if (!player->isInMarket()) {
		offerStatus << "Failed to load market";
		return;
	}

	if (player->isUIExhausted(1000)) {
		player->sendCancelMessage(RETURNVALUE_YOUAREEXHAUSTED);
		return;
	}

	MarketOfferEx offer = IOMarket::getOfferByCounter(timestamp, counter);
	if (offer.id == 0) {
		offerStatus << "Failed to load offer id";
		return;
	}

	const ItemType &it = Item::items[offer.itemId];
	if (it.id == 0) {
		offerStatus << "Failed to load item id";
		return;
	}

	if (amount == 0 || !it.stackable && amount > 2000 || it.stackable && amount > 64000 || amount > offer.amount) {
		offerStatus << "Invalid offer amount " << amount << " for player " << player->getName();
		return;
	}

	uint64_t totalPrice = offer.price * amount;

	// The player has an offer to by something and someone is going to sell to item type
	// so the market action is 'buy' as who created the offer is buying.
	if (offer.type == MARKETACTION_BUY) {
		std::shared_ptr<DepotLocker> depotLocker = player->getDepotLocker(player->getLastDepotId());
		if (depotLocker == nullptr) {
			offerStatus << "Depot locker is nullptr";
			return;
		}

		std::shared_ptr<Player> buyerPlayer = getPlayerByGUID(offer.playerId, true);
		if (!buyerPlayer) {
			offerStatus << "Failed to load buyer player " << player->getName();
			return;
		}

		if (!buyerPlayer->getAccount()) {
			player->sendTextMessage(MESSAGE_MARKET, "Cannot accept offer.");
			return;
		}

		if (player == buyerPlayer || player->getAccount() == buyerPlayer->getAccount()) {
			player->sendTextMessage(MESSAGE_MARKET, "You cannot accept your own offer.");
			return;
		}

		if (it.id == ITEM_STORE_COIN) {
			auto [transferableCoins, error] = player->getAccount()->getCoins(account::CoinType::TRANSFERABLE);

			if (error != account::ERROR_NO) {
				offerStatus << "Failed to load transferable coins for player " << player->getName();
				return;
			}

			if (amount > transferableCoins) {
				offerStatus << "Amount is greater than coins";
				return;
			}

			player->getAccount()->removeCoins(
				account::CoinType::TRANSFERABLE,
				amount,
				"Sold on Market"
			);
		} else {
			if (!removeOfferItems(player, depotLocker, it, amount, offer.tier, offerStatus)) {
				g_logger().error("[{}] failed to remove item with id {}, from player {}, errorcode: {}", __FUNCTION__, it.id, player->getName(), offerStatus.str());
				return;
			}
		}

		// If there is any error, then we will send the log and block the creation of the offer to avoid clone of items
		// The player may lose the item as it will have already been removed, but will not clone
		if (!offerStatus.str().empty()) {
			if (offerStatus.str() == "The item you tried to market is not correct. Check the item again.") {
				player->sendTextMessage(MESSAGE_MARKET, offerStatus.str());
			} else {
				player->sendTextMessage(MESSAGE_MARKET, "There was an error processing your offer, please contact the administrator.");
			}
			g_logger().error("{} - Player {} had an error creating an offer on the market, error code: {}", __FUNCTION__, player->getName(), offerStatus.str());
			player->sendMarketEnter(player->getLastDepotId());
			return;
		}

		player->setBankBalance(player->getBankBalance() + totalPrice);
		g_metrics().addCounter("balance_increase", totalPrice, { { "player", player->getName() }, { "context", "market_sale" } });

		if (it.id == ITEM_STORE_COIN) {
			buyerPlayer->getAccount()->addCoins(account::CoinType::TRANSFERABLE, amount, "Purchased on Market");
		} else if (it.stackable) {
			uint16_t tmpAmount = amount;
			while (tmpAmount > 0) {
				uint16_t stackCount = std::min<uint16_t>(it.stackSize, tmpAmount);
				std::shared_ptr<Item> item = Item::CreateItem(it.id, stackCount);
				if (internalAddItem(buyerPlayer->getInbox(), item, INDEX_WHEREEVER, FLAG_NOLIMIT) != RETURNVALUE_NOERROR) {
					offerStatus << "Failed to add player inbox stackable item for buy offer for player " << player->getName();

					break;
				}

				if (offer.tier > 0) {
					item->setAttribute(ItemAttribute_t::TIER, offer.tier);
				}

				tmpAmount -= stackCount;
			}
		} else {
			int32_t subType;
			if (it.charges != 0) {
				subType = it.charges;
			} else {
				subType = -1;
			}

			for (uint16_t i = 0; i < amount; ++i) {
				std::shared_ptr<Item> item = Item::CreateItem(it.id, subType);
				if (internalAddItem(buyerPlayer->getInbox(), item, INDEX_WHEREEVER, FLAG_NOLIMIT) != RETURNVALUE_NOERROR) {
					offerStatus << "Failed to add player inbox item for buy offer for player " << player->getName();

					break;
				}

				if (offer.tier > 0) {
					item->setAttribute(ItemAttribute_t::TIER, offer.tier);
				}
			}
		}

		if (buyerPlayer->isOffline()) {
			g_saveManager().savePlayer(buyerPlayer);
		}
	} else if (offer.type == MARKETACTION_SELL) {
		std::shared_ptr<Player> sellerPlayer = getPlayerByGUID(offer.playerId, true);
		if (!sellerPlayer) {
			offerStatus << "Failed to load seller player";
			return;
		}

		if (player == sellerPlayer || player->getAccount() == sellerPlayer->getAccount()) {
			player->sendTextMessage(MESSAGE_MARKET, "You cannot accept your own offer.");
			return;
		}

		if (totalPrice > (player->getBankBalance() + player->getMoney())) {
			return;
		}

		// Have enough money on the bank
		if (totalPrice <= player->getBankBalance()) {
			player->setBankBalance(player->getBankBalance() - totalPrice);
		} else {
			uint64_t remainsPrice = 0;
			remainsPrice = totalPrice - player->getBankBalance();
			player->setBankBalance(0);
			g_game().removeMoney(player, remainsPrice);
		}
		g_metrics().addCounter("balance_decrease", totalPrice, { { "player", player->getName() }, { "context", "market_purchase" } });

		if (it.id == ITEM_STORE_COIN) {
			player->getAccount()->addCoins(account::CoinType::TRANSFERABLE, amount, "Purchased on Market");
		} else if (it.stackable) {
			uint16_t tmpAmount = amount;
			while (tmpAmount > 0) {
				uint16_t stackCount = std::min<uint16_t>(it.stackSize, tmpAmount);
				std::shared_ptr<Item> item = Item::CreateItem(it.id, stackCount);
				if (
					// Init-statement
					auto ret = internalAddItem(player->getInbox(), item, INDEX_WHEREEVER, FLAG_NOLIMIT);
					// Condition
					ret != RETURNVALUE_NOERROR
				) {
					g_logger().error("{} - Create offer internal add item error code: {}", __FUNCTION__, getReturnMessage(ret));
					offerStatus << "Failed to add inbox stackable item for sell offer for player " << player->getName();

					break;
				}

				if (offer.tier > 0) {
					item->setAttribute(ItemAttribute_t::TIER, offer.tier);
				}

				tmpAmount -= stackCount;
			}
		} else {
			int32_t subType;
			if (it.charges != 0) {
				subType = it.charges;
			} else {
				subType = -1;
			}

			for (uint16_t i = 0; i < amount; ++i) {
				std::shared_ptr<Item> item = Item::CreateItem(it.id, subType);
				if (
					// Init-statement
					auto ret = internalAddItem(player->getInbox(), item, INDEX_WHEREEVER, FLAG_NOLIMIT);
					// Condition
					ret != RETURNVALUE_NOERROR
				) {
					offerStatus << "Failed to add inbox item for sell offer for player " << player->getName();

					break;
				}

				if (offer.tier > 0) {
					item->setAttribute(ItemAttribute_t::TIER, offer.tier);
				}
			}
		}

		sellerPlayer->setBankBalance(sellerPlayer->getBankBalance() + totalPrice);
		g_metrics().addCounter("balance_increase", totalPrice, { { "player", sellerPlayer->getName() }, { "context", "market_sale" } });
		if (it.id == ITEM_STORE_COIN) {
			sellerPlayer->getAccount()->registerCoinTransaction(account::CoinTransactionType::REMOVE, account::CoinType::TRANSFERABLE, amount, "Sold on Market");
		}

		if (it.id != ITEM_STORE_COIN) {
			player->onReceiveMail();
		}

		if (sellerPlayer->isOffline()) {
			g_saveManager().savePlayer(sellerPlayer);
		}
	}

	// Send market window again for update item stats and avoid item clone
	player->sendMarketEnter(player->getLastDepotId());

	if (!offerStatus.str().empty()) {
		player->sendTextMessage(MESSAGE_MARKET, "There was an error processing your offer, please contact the administrator.");
		g_logger().error("{} - Player {} had an error accepting an offer on the market, error code: {}", __FUNCTION__, player->getName(), offerStatus.str());
		return;
	}

	const int32_t marketOfferDuration = g_configManager().getNumber(MARKET_OFFER_DURATION, __FUNCTION__);

	IOMarket::appendHistory(player->getGUID(), (offer.type == MARKETACTION_BUY ? MARKETACTION_SELL : MARKETACTION_BUY), offer.itemId, amount, offer.price, time(nullptr), offer.tier, OFFERSTATE_ACCEPTEDEX);

	IOMarket::appendHistory(offer.playerId, offer.type, offer.itemId, amount, offer.price, time(nullptr), offer.tier, OFFERSTATE_ACCEPTED);

	offer.amount -= amount;

	if (offer.amount == 0) {
		IOMarket::deleteOffer(offer.id);
	} else {
		IOMarket::acceptOffer(offer.id, amount);
	}

	offer.timestamp += marketOfferDuration;
	player->sendMarketAcceptOffer(offer);
	// Exhausted for accept offer in the market
	player->updateUIExhausted();
	g_saveManager().savePlayer(player);
}

void Game::parsePlayerExtendedOpcode(uint32_t playerId, uint8_t opcode, const std::string &buffer) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	for (const auto creatureEvent : player->getCreatureEvents(CREATURE_EVENT_EXTENDED_OPCODE)) {
		creatureEvent->executeExtendedOpcode(player, opcode, buffer);
	}
}

void Game::forceRemoveCondition(uint32_t creatureId, ConditionType_t conditionType, ConditionId_t conditionId) {
	std::shared_ptr<Creature> creature = getCreatureByID(creatureId);
	if (!creature) {
		return;
	}

	creature->removeCondition(conditionType, conditionId, true);
}

void Game::sendOfflineTrainingDialog(std::shared_ptr<Player> player) {
	if (!player) {
		return;
	}

	if (!player->hasModalWindowOpen(offlineTrainingWindow.id)) {
		player->sendModalWindow(offlineTrainingWindow);
	}
}

void Game::playerAnswerModalWindow(uint32_t playerId, uint32_t modalWindowId, uint8_t button, uint8_t choice) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	if (!player->hasModalWindowOpen(modalWindowId)) {
		return;
	}

	player->onModalWindowHandled(modalWindowId);

	// offline training, hardcoded
	if (modalWindowId == std::numeric_limits<uint32_t>::max()) {
		if (button == 1) {
			if (choice == SKILL_SWORD || choice == SKILL_AXE || choice == SKILL_CLUB || choice == SKILL_DISTANCE || choice == SKILL_MAGLEVEL) {
				auto bedItem = player->getBedItem();
				if (bedItem && bedItem->sleep(player)) {
					player->setOfflineTrainingSkill(static_cast<int8_t>(choice));
					return;
				}
			}
		} else {
			player->sendTextMessage(MESSAGE_EVENT_ADVANCE, "Offline training aborted.");
		}

		player->setBedItem(nullptr);
	} else {
		for (const auto creatureEvent : player->getCreatureEvents(CREATURE_EVENT_MODALWINDOW)) {
			creatureEvent->executeModalWindow(player, modalWindowId, button, choice);
		}
	}
}

void Game::playerForgeFuseItems(uint32_t playerId, uint16_t itemId, uint8_t tier, bool usedCore, bool reduceTierLoss) {
	metrics::method_latency measure(__METHOD_NAME__);
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	if (player->isUIExhausted()) {
		player->sendCancelMessage(RETURNVALUE_YOUAREEXHAUSTED);
		return;
	}

	player->updateUIExhausted();

	uint8_t coreCount = (usedCore ? 1 : 0) + (reduceTierLoss ? 1 : 0);
	auto baseSuccess = static_cast<uint8_t>(g_configManager().getNumber(FORGE_BASE_SUCCESS_RATE, __FUNCTION__));
	auto bonusSuccess = static_cast<uint8_t>(g_configManager().getNumber(FORGE_BASE_SUCCESS_RATE, __FUNCTION__) + g_configManager().getNumber(FORGE_BONUS_SUCCESS_RATE, __FUNCTION__));
	auto roll = static_cast<uint8_t>(uniform_random(1, 100)) <= (usedCore ? bonusSuccess : baseSuccess);
	bool success = roll ? true : false;

	auto chance = uniform_random(0, 10000);
	uint8_t bonus = forgeBonus(chance);

	player->forgeFuseItems(itemId, tier, success, reduceTierLoss, bonus, coreCount);
}

void Game::playerForgeTransferItemTier(uint32_t playerId, uint16_t donorItemId, uint8_t tier, uint16_t receiveItemId) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	player->forgeTransferItemTier(donorItemId, tier, receiveItemId);
}

void Game::playerForgeResourceConversion(uint32_t playerId, uint8_t action) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	if (player->isUIExhausted()) {
		player->sendCancelMessage(RETURNVALUE_YOUAREEXHAUSTED);
		return;
	}

	player->updateUIExhausted();
	player->forgeResourceConversion(action);
}

void Game::playerBrowseForgeHistory(uint32_t playerId, uint8_t page) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	if (player->isUIExhausted()) {
		player->sendCancelMessage(RETURNVALUE_YOUAREEXHAUSTED);
		return;
	}

	player->updateUIExhausted();
	player->forgeHistory(page);
}

void Game::playerBosstiarySlot(uint32_t playerId, uint8_t slotId, uint32_t selectedBossId) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	if (player->isUIExhausted()) {
		player->sendCancelMessage(RETURNVALUE_YOUAREEXHAUSTED);
		return;
	}

	player->updateUIExhausted();

	uint32_t bossIdSlot = player->getSlotBossId(slotId);

	if (uint32_t boostedBossId = g_ioBosstiary().getBoostedBossId();
		selectedBossId == 0 && bossIdSlot != boostedBossId) {
		uint8_t removeTimes = player->getRemoveTimes();
		uint32_t removePrice = g_ioBosstiary().calculteRemoveBoss(removeTimes);
		g_game().removeMoney(player, removePrice, 0, true);
		g_metrics().addCounter("balance_decrease", removePrice, { { "player", player->getName() }, { "context", "bosstiary_remove" } });
		player->addRemoveTime();
	}

	player->setSlotBossId(slotId, selectedBossId);
}

void Game::playerSetMonsterPodium(uint32_t playerId, uint32_t monsterRaceId, const Position &pos, uint8_t stackPos, const uint16_t itemId, uint8_t direction, const std::pair<uint8_t, uint8_t> &podiumAndMonsterVisible) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player || pos.x == 0xFFFF) {
		return;
	}

	std::shared_ptr<Thing> thing = internalGetThing(player, pos, stackPos, itemId, STACKPOS_TOPDOWN_ITEM);
	if (!thing) {
		return;
	}

	std::shared_ptr<Item> item = thing->getItem();
	if (!item || item->getID() != itemId || !item->isPodium() || item->hasAttribute(ItemAttribute_t::UNIQUEID)) {
		player->sendCancelMessage(RETURNVALUE_NOTPOSSIBLE);
		return;
	}

	const auto tile = item->getParent() ? item->getParent()->getTile() : nullptr;
	if (!tile) {
		player->sendCancelMessage(RETURNVALUE_NOTPOSSIBLE);
		return;
	}

	if (!Position::areInRange<1, 1, 0>(pos, player->getPosition())) {
		if (stdext::arraylist<Direction> listDir(128);
			player->getPathTo(pos, listDir, 0, 1, true, false)) {
			g_dispatcher().addEvent(std::bind_front(&Game::playerAutoWalk, this, player->getID(), listDir.data()), "Game::playerAutoWalk");
			std::shared_ptr<Task> task = createPlayerTask(400, std::bind_front(&Game::playerBrowseField, this, playerId, pos), "Game::playerBrowseField");
			player->setNextWalkActionTask(task);
		} else {
			player->sendCancelMessage(RETURNVALUE_THEREISNOWAY);
		}
		return;
	}

	if (player->isUIExhausted()) {
		player->sendCancelMessage(RETURNVALUE_YOUAREEXHAUSTED);
		return;
	}

	if (g_configManager().getBoolean(ONLY_INVITED_CAN_MOVE_HOUSE_ITEMS, __FUNCTION__) && !InternalGame::playerCanUseItemOnHouseTile(player, item)) {
		player->sendCancelMessage(RETURNVALUE_NOTPOSSIBLE);
		return;
	}

	if (monsterRaceId != 0) {
		item->setCustomAttribute("PodiumMonsterRaceId", static_cast<int64_t>(monsterRaceId));
	} else if (auto podiumMonsterRace = item->getCustomAttribute("PodiumMonsterRaceId")) {
		monsterRaceId = static_cast<uint32_t>(podiumMonsterRace->getInteger());
	}

	const auto mType = g_monsters().getMonsterTypeByRaceId(static_cast<uint16_t>(monsterRaceId), itemId == ITEM_PODIUM_OF_VIGOUR);
	if (!mType) {
		player->sendCancelMessage(RETURNVALUE_NOTPOSSIBLE);
		g_logger().debug("[{}] player {} is trying to add invalid monster to podium {}", __FUNCTION__, player->getName(), item->getName());
		return;
	}

	const auto [podiumVisible, monsterVisible] = podiumAndMonsterVisible;
	bool changeTentuglyName = false;
	if (auto monsterOutfit = mType->info.outfit;
		(monsterOutfit.lookType != 0 || monsterOutfit.lookTypeEx != 0) && monsterVisible) {
		// "Tantugly's Head" boss have to send other looktype to the podium
		if (monsterOutfit.lookTypeEx == 35105) {
			monsterOutfit.lookTypeEx = 39003;
			changeTentuglyName = true;
		}
		item->setCustomAttribute("LookTypeEx", static_cast<int64_t>(monsterOutfit.lookTypeEx));
		item->setCustomAttribute("LookType", static_cast<int64_t>(monsterOutfit.lookType));
		item->setCustomAttribute("LookHead", static_cast<int64_t>(monsterOutfit.lookHead));
		item->setCustomAttribute("LookBody", static_cast<int64_t>(monsterOutfit.lookBody));
		item->setCustomAttribute("LookLegs", static_cast<int64_t>(monsterOutfit.lookLegs));
		item->setCustomAttribute("LookFeet", static_cast<int64_t>(monsterOutfit.lookFeet));
		item->setCustomAttribute("LookAddons", static_cast<int64_t>(monsterOutfit.lookAddons));
	} else {
		item->removeCustomAttribute("LookType");
	}

	item->setCustomAttribute("PodiumVisible", static_cast<int64_t>(podiumVisible));
	item->setCustomAttribute("LookDirection", static_cast<int64_t>(direction));
	item->setCustomAttribute("MonsterVisible", static_cast<int64_t>(monsterVisible));

	// Change Podium name
	if (monsterVisible) {
		std::ostringstream name;
		item->removeAttribute(ItemAttribute_t::NAME);
		name << item->getName() << " displaying ";
		if (changeTentuglyName) {
			name << "Tentugly";
		} else {
			name << mType->name;
		}
		item->setAttribute(ItemAttribute_t::NAME, name.str());
	} else {
		item->removeAttribute(ItemAttribute_t::NAME);
	}

	for (const auto &spectator : Spectators().find<Player>(pos, true)) {
		spectator->getPlayer()->sendUpdateTileItem(tile, pos, item);
	}

	player->updateUIExhausted();
}

void Game::playerRotatePodium(uint32_t playerId, const Position &pos, uint8_t stackPos, const uint16_t itemId) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	std::shared_ptr<Thing> thing = internalGetThing(player, pos, stackPos, itemId, STACKPOS_TOPDOWN_ITEM);
	if (!thing) {
		return;
	}

	std::shared_ptr<Item> item = thing->getItem();
	if (!item || item->getID() != itemId || item->hasAttribute(ItemAttribute_t::UNIQUEID)) {
		player->sendCancelMessage(RETURNVALUE_NOTPOSSIBLE);
		return;
	}

	if (pos.x != 0xFFFF && !Position::areInRange<1, 1, 0>(pos, player->getPosition())) {
		if (stdext::arraylist<Direction> listDir(128);
			player->getPathTo(pos, listDir, 0, 1, true, true)) {
			g_dispatcher().addEvent(std::bind_front(&Game::playerAutoWalk, this, player->getID(), listDir.data()), "Game::playerAutoWalk");

			std::shared_ptr<Task> task = createPlayerTask(400, std::bind_front(&Game::playerRotatePodium, this, playerId, pos, stackPos, itemId), "Game::playerRotatePodium");
			player->setNextWalkActionTask(task);
		} else {
			player->sendCancelMessage(RETURNVALUE_THEREISNOWAY);
		}
		return;
	}

	if (g_configManager().getBoolean(ONLY_INVITED_CAN_MOVE_HOUSE_ITEMS, __FUNCTION__) && !InternalGame::playerCanUseItemOnHouseTile(player, item)) {
		player->sendCancelMessage(RETURNVALUE_NOTPOSSIBLE);
		return;
	}

	auto podiumRaceIdAttribute = item->getCustomAttribute("PodiumMonsterRaceId");
	auto lookDirection = item->getCustomAttribute("LookDirection");
	auto podiumVisible = item->getCustomAttribute("PodiumVisible");
	auto monsterVisible = item->getCustomAttribute("MonsterVisible");

	auto podiumRaceId = podiumRaceIdAttribute ? static_cast<uint16_t>(podiumRaceIdAttribute->getInteger()) : 0;
	uint8_t directionValue;
	if (lookDirection) {
		directionValue = static_cast<uint8_t>(lookDirection->getInteger() >= 3 ? 0 : lookDirection->getInteger() + 1);
	} else {
		directionValue = 2;
	}
	auto isPodiumVisible = podiumVisible ? static_cast<bool>(podiumVisible->getInteger()) : false;
	bool isMonsterVisible = monsterVisible ? static_cast<bool>(monsterVisible->getInteger()) : false;

	// Rotate monster podium (bestiary or bosstiary) to the new direction
	bool isPodiumOfRenown = itemId == ITEM_PODIUM_OF_RENOWN1 || itemId == ITEM_PODIUM_OF_RENOWN2;
	if (!isPodiumOfRenown) {
		auto lookTypeExAttribute = item->getCustomAttribute("LookTypeEx");
		if (!isMonsterVisible || podiumRaceId == 0 || lookTypeExAttribute && lookTypeExAttribute->getInteger() == 39003) {
			player->sendCancelMessage(RETURNVALUE_NOTPOSSIBLE);
			return;
		}

		playerSetMonsterPodium(playerId, podiumRaceId, pos, stackPos, itemId, directionValue, std::make_pair(isPodiumVisible, isMonsterVisible));
		return;
	}

	// We retrieve the outfit information to be able to rotate the podium of renown in the new direction
	Outfit_t newOutfit;
	newOutfit.lookType = InternalGame::getCustomAttributeValue<uint16_t>(item, "LookType");
	newOutfit.lookAddons = InternalGame::getCustomAttributeValue<uint8_t>(item, "LookAddons");
	newOutfit.lookHead = InternalGame::getCustomAttributeValue<uint8_t>(item, "LookHead");
	newOutfit.lookBody = InternalGame::getCustomAttributeValue<uint8_t>(item, "LookBody");
	newOutfit.lookLegs = InternalGame::getCustomAttributeValue<uint8_t>(item, "LookLegs");
	newOutfit.lookFeet = InternalGame::getCustomAttributeValue<uint8_t>(item, "LookFeet");

	newOutfit.lookMount = InternalGame::getCustomAttributeValue<uint16_t>(item, "LookMount");
	newOutfit.lookMountHead = InternalGame::getCustomAttributeValue<uint8_t>(item, "LookMountHead");
	newOutfit.lookMountBody = InternalGame::getCustomAttributeValue<uint8_t>(item, "LookMountBody");
	newOutfit.lookMountLegs = InternalGame::getCustomAttributeValue<uint8_t>(item, "LookMountLegs");
	newOutfit.lookMountFeet = InternalGame::getCustomAttributeValue<uint8_t>(item, "LookMountFeet");
	if (newOutfit.lookType == 0 && newOutfit.lookMount == 0) {
		player->sendCancelMessage(RETURNVALUE_NOTPOSSIBLE);
		return;
	}

	playerSetShowOffSocket(player->getID(), newOutfit, pos, stackPos, itemId, isPodiumVisible, directionValue);
}

void Game::playerRequestInventoryImbuements(uint32_t playerId, bool isTrackerOpen) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player || player->isRemoved()) {
		return;
	}

	player->imbuementTrackerWindowOpen = isTrackerOpen;
	if (!player->imbuementTrackerWindowOpen) {
		return;
	}

	std::map<Slots_t, std::shared_ptr<Item>> itemsWithImbueSlotMap;
	for (uint8_t inventorySlot = CONST_SLOT_FIRST; inventorySlot <= CONST_SLOT_LAST; ++inventorySlot) {
		auto item = player->getInventoryItem(static_cast<Slots_t>(inventorySlot));
		if (!item) {
			continue;
		}

		uint8_t imbuementSlot = item->getImbuementSlot();
		for (uint8_t slot = 0; slot < imbuementSlot; slot++) {
			ImbuementInfo imbuementInfo;
			if (!item->getImbuementInfo(slot, &imbuementInfo)) {
				continue;
			}
		}

		itemsWithImbueSlotMap[static_cast<Slots_t>(inventorySlot)] = item;
	}

	player->sendInventoryImbuements(itemsWithImbueSlotMap);
}

void Game::playerOpenWheel(uint32_t playerId, uint32_t ownerId) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	if (playerId != ownerId) {
		g_logger().error("[{}] player {} is trying to open wheel of another player", __FUNCTION__, player->getName());
		return;
	}

	if (player->isUIExhausted()) {
		player->sendCancelMessage(RETURNVALUE_YOUAREEXHAUSTED);
		return;
	}

	player->wheel()->sendOpenWheelWindow(ownerId);
	player->updateUIExhausted();
}

void Game::playerSaveWheel(uint32_t playerId, NetworkMessage &msg) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	if (player->isUIExhausted()) {
		player->sendCancelMessage(RETURNVALUE_YOUAREEXHAUSTED);
		return;
	}

	player->wheel()->saveSlotPointsOnPressSaveButton(msg);
	player->updateUIExhausted();
}

/* Player Methods end
********************/

void Game::updatePlayerSaleItems(uint32_t playerId) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	std::map<uint16_t, uint16_t> inventoryMap;
	player->sendSaleItemList(player->getAllSaleItemIdAndCount(inventoryMap));
	player->setScheduledSaleUpdate(false);
}

void Game::addPlayer(std::shared_ptr<Player> player) {
	const std::string &lowercase_name = asLowerCaseString(player->getName());
	mappedPlayerNames[lowercase_name] = player;
	wildcardTree.insert(lowercase_name);
	players[player->getID()] = player;
}

void Game::removePlayer(std::shared_ptr<Player> player) {
	const std::string &lowercase_name = asLowerCaseString(player->getName());
	mappedPlayerNames.erase(lowercase_name);
	wildcardTree.remove(lowercase_name);
	players.erase(player->getID());
}

void Game::addNpc(std::shared_ptr<Npc> npc) {
	npcs[npc->getID()] = npc;
}

void Game::removeNpc(std::shared_ptr<Npc> npc) {
	npcs.erase(npc->getID());
}

void Game::addMonster(std::shared_ptr<Monster> monster) {
	monsters[monster->getID()] = monster;
}

void Game::removeMonster(std::shared_ptr<Monster> monster) {
	monsters.erase(monster->getID());
}

std::shared_ptr<Guild> Game::getGuild(uint32_t id, bool allowOffline /* = flase */) const {
	auto it = guilds.find(id);
	if (it == guilds.end()) {
		if (allowOffline) {
			return IOGuild::loadGuild(id);
		}
		return nullptr;
	}
	return it->second;
}

std::shared_ptr<Guild> Game::getGuildByName(const std::string &name, bool allowOffline /* = flase */) const {
	auto id = IOGuild::getGuildIdByName(name);
	auto it = guilds.find(id);
	if (it == guilds.end()) {
		if (allowOffline) {
			return IOGuild::loadGuild(id);
		}
		return nullptr;
	}
	return it->second;
}

void Game::addGuild(const std::shared_ptr<Guild> guild) {
	if (!guild) {
		return;
	}
	guilds[guild->getId()] = guild;
}

void Game::removeGuild(uint32_t guildId) {
	auto it = guilds.find(guildId);
	if (it != guilds.end()) {
		g_saveManager().saveGuild(it->second);
	}
	guilds.erase(guildId);
}

void Game::internalRemoveItems(const std::vector<std::shared_ptr<Item>> &itemVector, uint32_t amount, bool stackable) {
	if (stackable) {
		for (std::shared_ptr<Item> item : itemVector) {
			if (item->getItemCount() > amount) {
				internalRemoveItem(item, amount);
				break;
			} else {
				amount -= item->getItemCount();
				internalRemoveItem(item);
			}
		}
	} else {
		for (std::shared_ptr<Item> item : itemVector) {
			internalRemoveItem(item);
		}
	}
}

std::shared_ptr<BedItem> Game::getBedBySleeper(uint32_t guid) const {
	auto it = bedSleepersMap.find(guid);
	if (it == bedSleepersMap.end()) {
		return nullptr;
	}
	return it->second;
}

void Game::setBedSleeper(std::shared_ptr<BedItem> bed, uint32_t guid) {
	bedSleepersMap[guid] = bed;
}

void Game::removeBedSleeper(uint32_t guid) {
	auto it = bedSleepersMap.find(guid);
	if (it != bedSleepersMap.end()) {
		bedSleepersMap.erase(it);
	}
}

std::shared_ptr<Item> Game::getUniqueItem(uint16_t uniqueId) {
	auto it = uniqueItems.find(uniqueId);
	if (it == uniqueItems.end()) {
		return nullptr;
	}
	return it->second;
}

bool Game::addUniqueItem(uint16_t uniqueId, std::shared_ptr<Item> item) {
	auto result = uniqueItems.emplace(uniqueId, item);
	if (!result.second) {
		g_logger().warn("Duplicate unique id: {}", uniqueId);
	}
	return result.second;
}

void Game::removeUniqueItem(uint16_t uniqueId) {
	auto it = uniqueItems.find(uniqueId);
	if (it != uniqueItems.end()) {
		uniqueItems.erase(it);
	}
}

bool Game::hasEffect(uint16_t effectId) {
	for (uint16_t i = CONST_ME_NONE; i <= CONST_ME_LAST; i++) {
		MagicEffectClasses effect = static_cast<MagicEffectClasses>(i);
		if (effect == effectId) {
			return true;
		}
	}
	return false;
}

bool Game::hasDistanceEffect(uint16_t effectId) {
	for (uint16_t i = CONST_ANI_NONE; i <= CONST_ANI_LAST; i++) {
		ShootType_t effect = static_cast<ShootType_t>(i);
		if (effect == effectId) {
			return true;
		}
	}
	return false;
}

void Game::createLuaItemsOnMap() {
	for (const auto [position, itemId] : mapLuaItemsStored) {
		std::shared_ptr<Item> item = Item::CreateItem(itemId, 1);
		if (!item) {
			g_logger().warn("[Game::createLuaItemsOnMap] - Cannot create item with id {}", itemId);
			continue;
		}

		if (position.x != 0) {
			std::shared_ptr<Tile> tile = g_game().map.getTile(position);
			if (!tile) {
				g_logger().warn("[Game::createLuaItemsOnMap] - Tile is wrong or not found position: {}", position.toString());

				continue;
			}

			// If the item already exists on the map, then ignore it and send warning
			if (g_game().findItemOfType(tile, itemId, false, -1)) {
				g_logger().warn("[Game::createLuaItemsOnMap] - Cannot create item with id {} on position {}, item already exists", itemId, position.toString());
				continue;
			}

			g_game().internalAddItem(tile, item, INDEX_WHEREEVER, FLAG_NOLIMIT);
		}
	}
}

void Game::sendUpdateCreature(std::shared_ptr<Creature> creature) {
	if (!creature) {
		return;
	}

	for (const auto &spectator : Spectators().find<Player>(creature->getPosition(), true)) {
		spectator->getPlayer()->sendUpdateCreature(creature);
	}
}

uint32_t Game::makeInfluencedMonster() {
	if (auto influencedLimit = g_configManager().getNumber(FORGE_INFLUENCED_CREATURES_LIMIT, __FUNCTION__);
		// Condition
		forgeableMonsters.empty() || influencedMonsters.size() >= influencedLimit) {
		return 0;
	}

	if (forgeableMonsters.empty()) {
		return 0;
	}

	auto maxTries = forgeableMonsters.size();
	uint16_t tries = 0;
	std::shared_ptr<Monster> monster = nullptr;
	while (true) {
		if (tries == maxTries) {
			return 0;
		}

		tries++;

		auto random = static_cast<uint32_t>(normal_random(0, static_cast<int32_t>(forgeableMonsters.size() - 1)));
		auto monsterId = forgeableMonsters.at(random);
		monster = getMonsterByID(monsterId);
		if (monster == nullptr) {
			continue;
		}

		// Avoiding replace forgeable monster with another
		if (monster->getForgeStack() == 0) {
			auto it = std::ranges::find(forgeableMonsters.begin(), forgeableMonsters.end(), monsterId);
			if (it == forgeableMonsters.end()) {
				monster = nullptr;
				continue;
			}
			forgeableMonsters.erase(it);
			break;
		}
	}

	if (monster && monster->canBeForgeMonster()) {
		monster->setMonsterForgeClassification(ForgeClassifications_t::FORGE_INFLUENCED_MONSTER);
		monster->configureForgeSystem();
		influencedMonsters.insert(monster->getID());
		return monster->getID();
	}

	return 0;
}

uint32_t Game::makeFiendishMonster(uint32_t forgeableMonsterId /* = 0*/, bool createForgeableMonsters /* = false*/) {
	if (createForgeableMonsters) {
		forgeableMonsters.clear();
		// If the forgeable monsters haven't been created
		// Then we'll create them so they don't return in the next if (forgeableMonsters.empty())
		for (auto [monsterId, monster] : monsters) {
			auto monsterTile = monster->getTile();
			if (!monster || !monsterTile) {
				continue;
			}

			if (monster->canBeForgeMonster() && !monsterTile->hasFlag(TILESTATE_NOLOGOUT)) {
				forgeableMonsters.push_back(monster->getID());
			}
		}
		for (const auto monsterId : getFiendishMonsters()) {
			// If the fiendish is no longer on the map, we remove it from the vector
			auto monster = getMonsterByID(monsterId);
			if (!monster) {
				removeFiendishMonster(monsterId);
				continue;
			}

			// If you're trying to create a new fiendish and it's already max size, let's remove one of them
			if (getFiendishMonsters().size() >= 3) {
				monster->clearFiendishStatus();
				removeFiendishMonster(monsterId);
				break;
			}
		}
	}

	if (auto fiendishLimit = g_configManager().getNumber(FORGE_FIENDISH_CREATURES_LIMIT, __FUNCTION__);
		// Condition
		forgeableMonsters.empty() || fiendishMonsters.size() >= fiendishLimit) {
		return 0;
	}

	auto maxTries = forgeableMonsters.size();
	uint16_t tries = 0;
	std::shared_ptr<Monster> monster = nullptr;
	while (true) {
		if (tries == maxTries) {
			return 0;
		}

		tries++;

		auto random = static_cast<uint32_t>(uniform_random(0, static_cast<int32_t>(forgeableMonsters.size() - 1)));
		uint32_t fiendishMonsterId = forgeableMonsterId;
		if (fiendishMonsterId == 0) {
			fiendishMonsterId = forgeableMonsters.at(random);
		}
		monster = getMonsterByID(fiendishMonsterId);
		if (monster == nullptr) {
			continue;
		}

		// Avoiding replace forgeable monster with another
		if (monster->getForgeStack() == 0) {
			auto it = std::find(forgeableMonsters.begin(), forgeableMonsters.end(), fiendishMonsterId);
			if (it == forgeableMonsters.end()) {
				monster = nullptr;
				continue;
			}
			forgeableMonsters.erase(it);
			break;
		}
	}

	// Get interval time to fiendish
	std::string saveIntervalType = g_configManager().getString(FORGE_FIENDISH_INTERVAL_TYPE, __FUNCTION__);
	auto saveIntervalConfigTime = std::atoi(g_configManager().getString(FORGE_FIENDISH_INTERVAL_TIME, __FUNCTION__).c_str());
	int intervalTime = 0;
	time_t timeToChangeFiendish;
	if (saveIntervalType == "second") {
		intervalTime = 1000;
		timeToChangeFiendish = 1;
	} else if (saveIntervalType == "minute") {
		intervalTime = 60 * 1000;
		timeToChangeFiendish = 60;
	} else if (saveIntervalType == "hour") {
		intervalTime = 60 * 60 * 1000;
		timeToChangeFiendish = 3600;
	} else {
		timeToChangeFiendish = 3600;
	}

	uint32_t finalTime = 0;
	if (intervalTime == 0) {
		g_logger().warn("Fiendish interval type is wrong, setting default time to 1h");
		finalTime = 3600 * 1000;
	} else {
		finalTime = static_cast<uint32_t>(saveIntervalConfigTime * intervalTime);
	}

	if (monster && monster->canBeForgeMonster()) {
		monster->setMonsterForgeClassification(ForgeClassifications_t::FORGE_FIENDISH_MONSTER);
		monster->configureForgeSystem();
		monster->setTimeToChangeFiendish(timeToChangeFiendish + getTimeNow());
		fiendishMonsters.insert(monster->getID());

		auto schedulerTask = createPlayerTask(
			finalTime,
			std::bind_front(&Game::updateFiendishMonsterStatus, this, monster->getID(), monster->getName()),
			"Game::updateFiendishMonsterStatus"
		);
		forgeMonsterEventIds[monster->getID()] = g_dispatcher().scheduleEvent(schedulerTask);
		return monster->getID();
	}

	return 0;
}

void Game::updateFiendishMonsterStatus(uint32_t monsterId, const std::string &monsterName) {
	std::shared_ptr<Monster> monster = getMonsterByID(monsterId);
	if (!monster) {
		g_logger().warn("[{}] Failed to update monster with id {} and name {}, monster not found", __FUNCTION__, monsterId, monsterName);
		return;
	}

	monster->clearFiendishStatus();
	removeFiendishMonster(monsterId, false);
	makeFiendishMonster();
}

bool Game::removeForgeMonster(uint32_t id, ForgeClassifications_t monsterForgeClassification, bool create) {
	if (monsterForgeClassification == ForgeClassifications_t::FORGE_FIENDISH_MONSTER) {
		removeFiendishMonster(id, create);
	} else if (monsterForgeClassification == ForgeClassifications_t::FORGE_INFLUENCED_MONSTER) {
		removeInfluencedMonster(id, create);
	}

	return true;
}

bool Game::removeInfluencedMonster(uint32_t id, bool create /* = false*/) {
	if (auto find = influencedMonsters.find(id);
		// Condition
		find != influencedMonsters.end()) {
		influencedMonsters.erase(find);

		if (create) {
			g_dispatcher().scheduleEvent(200 * 1000, std::bind_front(&Game::makeInfluencedMonster, this), "Game::makeInfluencedMonster");
		}
	} else {
		g_logger().warn("[Game::removeInfluencedMonster] - Failed to remove a Influenced Monster, error code: monster id not exist in the influenced monsters map");
	}
	return false;
}

bool Game::removeFiendishMonster(uint32_t id, bool create /* = true*/) {
	if (auto find = fiendishMonsters.find(id);
		// Condition
		find != fiendishMonsters.end()) {
		fiendishMonsters.erase(find);
		checkForgeEventId(id);

		if (create) {
			g_dispatcher().scheduleEvent(300 * 1000, std::bind_front(&Game::makeFiendishMonster, this, 0, false), "Game::makeFiendishMonster");
		}
	} else {
		g_logger().warn("[Game::removeFiendishMonster] - Failed to remove a Fiendish Monster, error code: monster id not exist in the fiendish monsters map");
	}

	return false;
}

void Game::updateForgeableMonsters() {
	forgeableMonsters.clear();
	for (auto [monsterId, monster] : monsters) {
		auto monsterTile = monster->getTile();
		if (!monsterTile) {
			continue;
		}

		if (monster->canBeForgeMonster() && !monsterTile->hasFlag(TILESTATE_NOLOGOUT)) {
			forgeableMonsters.push_back(monster->getID());
		}
	}

	for (const auto monsterId : getFiendishMonsters()) {
		if (!getMonsterByID(monsterId)) {
			removeFiendishMonster(monsterId);
		}
	}

	uint32_t fiendishLimit = g_configManager().getNumber(FORGE_FIENDISH_CREATURES_LIMIT, __FUNCTION__); // Fiendish Creatures limit
	if (fiendishMonsters.size() < fiendishLimit) {
		createFiendishMonsters();
	}
}

void Game::createFiendishMonsters() {
	uint32_t created = 0;
	uint32_t fiendishLimit = g_configManager().getNumber(FORGE_FIENDISH_CREATURES_LIMIT, __FUNCTION__); // Fiendish Creatures limit
	while (fiendishMonsters.size() < fiendishLimit) {
		if (fiendishMonsters.size() >= fiendishLimit) {
			g_logger().warn("[{}] - Returning in creation of Fiendish, size: {}, max is: {}.", __FUNCTION__, fiendishMonsters.size(), fiendishLimit);
			break;
		}

		if (auto ret = makeFiendishMonster();
			// Condition
			ret == 0) {
			return;
		}

		created++;
	}
}

void Game::createInfluencedMonsters() {
	uint32_t created = 0;
	uint32_t influencedLimit = g_configManager().getNumber(FORGE_INFLUENCED_CREATURES_LIMIT, __FUNCTION__);
	while (created < influencedLimit) {
		if (influencedMonsters.size() >= influencedLimit) {
			g_logger().warn("[{}] - Returning in creation of Influenced, size: {}, max is: {}.", __FUNCTION__, influencedMonsters.size(), influencedLimit);
			break;
		}

		if (auto ret = makeInfluencedMonster();
			// If condition
			ret == 0) {
			return;
		}

		created++;
	}
}

void Game::checkForgeEventId(uint32_t monsterId) {
	auto find = forgeMonsterEventIds.find(monsterId);
	if (find != forgeMonsterEventIds.end()) {
		g_dispatcher().stopEvent(find->second);
		forgeMonsterEventIds.erase(find);
	}
}

bool Game::addInfluencedMonster(std::shared_ptr<Monster> monster) {
	if (monster && monster->canBeForgeMonster()) {
		if (auto maxInfluencedMonsters = static_cast<uint32_t>(g_configManager().getNumber(FORGE_INFLUENCED_CREATURES_LIMIT, __FUNCTION__));
			// If condition
			(influencedMonsters.size() + 1) > maxInfluencedMonsters) {
			return false;
		}

		monster->setMonsterForgeClassification(ForgeClassifications_t::FORGE_INFLUENCED_MONSTER);
		monster->configureForgeSystem();
		influencedMonsters.insert(monster->getID());
		return true;
	}
	return false;
}

bool Game::addItemStoreInbox(std::shared_ptr<Player> player, uint32_t itemId) {
	std::shared_ptr<Item> decoKit = Item::CreateItem(ITEM_DECORATION_KIT, 1);
	if (!decoKit) {
		return false;
	}
	const ItemType &itemType = Item::items[itemId];
	std::string description = fmt::format("Unwrap it in your own house to create a <{}>.", itemType.name);
	decoKit->setAttribute(ItemAttribute_t::DESCRIPTION, description);
	decoKit->setCustomAttribute("unWrapId", static_cast<int64_t>(itemId));

	std::shared_ptr<Thing> thing = player->getThing(CONST_SLOT_STORE_INBOX);
	if (!thing) {
		return false;
	}

	std::shared_ptr<Item> inboxItem = thing->getItem();
	if (!inboxItem) {
		return false;
	}

	std::shared_ptr<Container> inboxContainer = inboxItem->getContainer();
	if (!inboxContainer) {
		return false;
	}

	if (internalAddItem(inboxContainer, decoKit) != RETURNVALUE_NOERROR) {
		inboxContainer->internalAddThing(decoKit);
	}

	return true;
}

void Game::addPlayerUniqueLogin(std::shared_ptr<Player> player) {
	if (!player) {
		g_logger().error("Attempted to add null player to unique player names list");
		return;
	}

	const std::string &lowercase_name = asLowerCaseString(player->getName());
	m_uniqueLoginPlayerNames[lowercase_name] = player;
}

std::shared_ptr<Player> Game::getPlayerUniqueLogin(const std::string &playerName) const {
	if (playerName.empty()) {
		g_logger().error("Attempted to get player with empty name string");
		return nullptr;
	}

	auto it = m_uniqueLoginPlayerNames.find(asLowerCaseString(playerName));
	return (it != m_uniqueLoginPlayerNames.end()) ? it->second.lock() : nullptr;
}

void Game::removePlayerUniqueLogin(const std::string &playerName) {
	if (playerName.empty()) {
		g_logger().error("Attempted to remove player with empty name string from unique player names list");
		return;
	}

	const std::string &lowercase_name = asLowerCaseString(playerName);
	m_uniqueLoginPlayerNames.erase(lowercase_name);
}

void Game::removePlayerUniqueLogin(std::shared_ptr<Player> player) {
	if (!player) {
		g_logger().error("Attempted to remove null player from unique player names list.");
		return;
	}

	const std::string &lowercaseName = asLowerCaseString(player->getName());
	m_uniqueLoginPlayerNames.erase(lowercaseName);
}

void Game::playerCheckActivity(const std::string &playerName, int interval) {
	std::shared_ptr<Player> player = getPlayerUniqueLogin(playerName);
	if (!player) {
		return;
	}

	if (player->getIP() == 0) {
		g_game().removePlayerUniqueLogin(playerName);
		IOLoginData::updateOnlineStatus(player->guid, false);
		g_logger().info("Player with name '{}' has logged out due to exited in death screen", player->getName());
		player->disconnect();
		return;
	}

	if (!player->isDead() || player->client == nullptr) {
		return;
	}

	if (!player->isAccessPlayer()) {
		player->m_deathTime += interval;
		const int32_t kickAfterMinutes = g_configManager().getNumber(KICK_AFTER_MINUTES, __FUNCTION__);
		if (player->m_deathTime > (kickAfterMinutes * 60000) + 60000) {
			g_logger().info("Player with name '{}' has logged out due to inactivity after death", player->getName());
			g_game().removePlayerUniqueLogin(playerName);
			IOLoginData::updateOnlineStatus(player->guid, false);
			player->disconnect();
			return;
		}
	}

	g_dispatcher().scheduleEvent(1000, std::bind(&Game::playerCheckActivity, this, playerName, interval), "Game::playerCheckActivity");
}

void Game::playerRewardChestCollect(uint32_t playerId, const Position &pos, uint16_t itemId, uint8_t stackPos, uint32_t maxMoveItems /* = 0*/) {
	std::shared_ptr<Player> player = getPlayerByID(playerId);
	if (!player) {
		return;
	}

	std::shared_ptr<Thing> thing = internalGetThing(player, pos, stackPos, itemId, STACKPOS_FIND_THING);
	if (!thing) {
		player->sendCancelMessage(RETURNVALUE_NOTPOSSIBLE);
		return;
	}

	auto item = thing->getItem();
	if (!item || item->getID() != ITEM_REWARD_CHEST || !item->getContainer()) {
		player->sendCancelMessage(RETURNVALUE_NOTPOSSIBLE);
		return;
	}

	if (auto function = std::bind(&Game::playerRewardChestCollect, this, player->getID(), pos, itemId, stackPos, maxMoveItems);
		player->canAutoWalk(item->getPosition(), function)) {
		return;
	}

	// Updates the parent of the reward chest and reward containers to avoid memory usage after cleaning
	auto playerRewardChest = player->getRewardChest();
	if (playerRewardChest && playerRewardChest->empty()) {
		player->sendCancelMessage(RETURNVALUE_REWARDCHESTISEMPTY);
		return;
	}

	playerRewardChest->setParent(item->getContainer()->getParent()->getTile());
	for (const auto &[mapRewardId, reward] : player->rewardMap) {
		reward->setParent(playerRewardChest);
	}

	std::scoped_lock<std::mutex> lock(player->quickLootMutex);

	ReturnValue returnValue = collectRewardChestItems(player, maxMoveItems);
	if (returnValue != RETURNVALUE_NOERROR) {
		player->sendCancelMessage(returnValue);
	}
}

bool Game::tryRetrieveStashItems(std::shared_ptr<Player> player, std::shared_ptr<Item> item) {
	return internalCollectLootItems(player, item, OBJECTCATEGORY_STASHRETRIEVE) == RETURNVALUE_NOERROR;
}

std::unique_ptr<IOWheel> &Game::getIOWheel() {
	return m_IOWheel;
}

const std::unique_ptr<IOWheel> &Game::getIOWheel() const {
	return m_IOWheel;
}

void Game::transferHouseItemsToDepot() {
	if (!g_configManager().getBoolean(TOGGLE_HOUSE_TRANSFER_ON_SERVER_RESTART, __FUNCTION__)) {
		return;
	}

	if (!transferHouseItemsToPlayer.empty()) {
		g_logger().info("Initializing house transfer items");
	}

	uint16_t transferSuccess = 0;
	for (const auto &[houseId, playerGuid] : transferHouseItemsToPlayer) {
		auto house = map.houses.getHouse(houseId);
		if (house) {
			auto offlinePlayer = std::make_shared<Player>(nullptr);
			if (!IOLoginData::loadPlayerById(offlinePlayer, playerGuid)) {
				continue;
			}

			if (!offlinePlayer) {
				continue;
			}

			g_logger().info("Tranfering items to the inbox from player '{}'", offlinePlayer->getName());
			if (house->tryTransferOwnership(offlinePlayer, true)) {
				transferSuccess++;
				house->setNewOwnerGuid(-1, true);
			}
		}
	}
	if (transferSuccess > 0) {
		g_logger().info("Finished house transfer items from '{}' players", transferSuccess);
		transferHouseItemsToPlayer.clear();
		Map::save();
	}
}

void Game::setTransferPlayerHouseItems(uint32_t houseId, uint32_t playerId) {
	transferHouseItemsToPlayer[houseId] = playerId;
}

template <typename T>
std::vector<T> setDifference(const std::unordered_set<T> &setA, const std::unordered_set<T> &setB) {
	std::vector<T> setResult;
	setResult.reserve(setA.size());

	for (const auto &elem : setA) {
		if (!setB.contains(elem)) {
			setResult.emplace_back(elem);
		}
	}

	return setResult;
}

ReturnValue Game::beforeCreatureZoneChange(std::shared_ptr<Creature> creature, const std::unordered_set<std::shared_ptr<Zone>> &fromZones, const std::unordered_set<std::shared_ptr<Zone>> &toZones, bool force /* = false*/) const {
	if (!creature) {
		return RETURNVALUE_NOTPOSSIBLE;
	}

	// fromZones - toZones = zones that creature left
	const auto &zonesLeaving = setDifference(fromZones, toZones);
	// toZones - fromZones = zones that creature entered
	const auto &zonesEntering = setDifference(toZones, fromZones);

	if (zonesLeaving.empty() && zonesEntering.empty()) {
		return RETURNVALUE_NOERROR;
	}

	for (const auto &zone : zonesLeaving) {
		bool allowed = g_callbacks().checkCallback(EventCallback_t::zoneBeforeCreatureLeave, &EventCallback::zoneBeforeCreatureLeave, zone, creature);
		if (!force && !allowed) {
			return RETURNVALUE_NOTPOSSIBLE;
		}
	}

	for (const auto &zone : zonesEntering) {
		bool allowed = g_callbacks().checkCallback(EventCallback_t::zoneBeforeCreatureEnter, &EventCallback::zoneBeforeCreatureEnter, zone, creature);
		if (!force && !allowed) {
			return RETURNVALUE_NOTPOSSIBLE;
		}
	}
	return RETURNVALUE_NOERROR;
}

void Game::afterCreatureZoneChange(std::shared_ptr<Creature> creature, const std::unordered_set<std::shared_ptr<Zone>> &fromZones, const std::unordered_set<std::shared_ptr<Zone>> &toZones) const {
	if (!creature) {
		return;
	}

	// fromZones - toZones = zones that creature left
	const auto &zonesLeaving = setDifference(fromZones, toZones);
	// toZones - fromZones = zones that creature entered
	const auto &zonesEntering = setDifference(toZones, fromZones);

	for (const auto &zone : zonesLeaving) {
		zone->creatureRemoved(creature);
	}
	for (const auto &zone : zonesEntering) {
		zone->creatureAdded(creature);
	}

	for (const auto &zone : zonesLeaving) {
		g_callbacks().executeCallback(EventCallback_t::zoneAfterCreatureLeave, &EventCallback::zoneAfterCreatureLeave, zone, creature);
	}
	for (const auto &zone : zonesEntering) {
		g_callbacks().executeCallback(EventCallback_t::zoneAfterCreatureEnter, &EventCallback::zoneAfterCreatureEnter, zone, creature);
	}
}
