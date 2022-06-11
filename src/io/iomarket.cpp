/**
 * Canary - A free and open-source MMORPG server emulator
 * Copyright (©) 2019-2022 OpenTibiaBR <opentibiabr@outlook.com>
 * Repository: https://github.com/opentibiabr/canary
 * License: https://github.com/opentibiabr/canary/blob/main/LICENSE
 * Contributors: https://github.com/opentibiabr/canary/graphs/contributors
 * Website: https://docs.opentibiabr.org/
*/

#include "otpch.h"

#include "io/iomarket.h"

#include "database/databasetasks.h"
#include "io/iologindata.h"
#include "game/game.h"
#include "game/scheduling/scheduler.h"


MarketOfferList IOMarket::getActiveOffers(MarketAction_t action, uint16_t itemId)
{
	MarketOfferList offerList;

	std::ostringstream query;
	query << "SELECT `id`, `amount`, `price`, `created`, `anonymous`, (SELECT `name` FROM `players` WHERE `id` = `player_id`) AS `player_name` FROM `market_offers` WHERE `sale` = " << action << " AND `itemtype` = " << itemId;

	DBResult_ptr result = Database::getInstance().storeQuery(query.str());
	if (!result) {
		return offerList;
	}

	const int32_t marketOfferDuration = g_configManager().getNumber(MARKET_OFFER_DURATION);

	do {
		MarketOffer offer;
		offer.amount = result->getU16("amount");
		offer.price = result->getU32("price");
		offer.timestamp = result->getU32("created") + marketOfferDuration;
		offer.counter = result->getU32("id") & 0xFFFF;
		if (result->getU16("anonymous") == 0) {
			offer.playerName = result->getString("player_name");
		} else {
			offer.playerName = "Anonymous";
		}
		offerList.push_back(offer);
	} while (result->next());
	return offerList;
}

MarketOfferList IOMarket::getOwnOffers(MarketAction_t action, uint32_t playerId)
{
	MarketOfferList offerList;

	const int32_t marketOfferDuration = g_configManager().getNumber(MARKET_OFFER_DURATION);

	std::ostringstream query;
	query << "SELECT `id`, `amount`, `price`, `created`, `itemtype` FROM `market_offers` WHERE `player_id` = " << playerId << " AND `sale` = " << action;

	DBResult_ptr result = Database::getInstance().storeQuery(query.str());
	if (!result) {
		return offerList;
	}

	do {
		MarketOffer offer;
		offer.amount = result->getU16("amount");
		offer.price = result->getU32("price");
		offer.timestamp = result->getU32("created") + marketOfferDuration;
		offer.counter = result->getU32("id") & 0xFFFF;
		offer.itemId = result->getU16("itemtype");
		offerList.push_back(offer);
	} while (result->next());
	return offerList;
}

HistoryMarketOfferList IOMarket::getOwnHistory(MarketAction_t action, uint32_t playerId)
{
	HistoryMarketOfferList offerList;

	std::ostringstream query;
	query << "SELECT `itemtype`, `amount`, `price`, `expires_at`, `state` FROM `market_history` WHERE `player_id` = " << playerId << " AND `sale` = " << action;

	DBResult_ptr result = Database::getInstance().storeQuery(query.str());
	if (!result) {
		return offerList;
	}

	do {
		HistoryMarketOffer offer;
		offer.itemId = result->getU16("itemtype");
		offer.amount = result->getU16("amount");
		offer.price = result->getU32("price");
		offer.timestamp = result->getU32("expires_at");

		auto offerState = static_cast<MarketOfferState_t>(result->getU16("state"));
		if (offerState == OFFERSTATE_ACCEPTEDEX) {
			offerState = OFFERSTATE_ACCEPTED;
		}

		offer.state = offerState;

		offerList.push_back(offer);
	} while (result->next());
	return offerList;
}

void IOMarket::processExpiredOffers(DBResult_ptr result, bool)
{
	if (!result) {
		return;
	}

	do {
		if (!IOMarket::moveOfferToHistory(result->getU32("id"), OFFERSTATE_EXPIRED)) {
			continue;
		}

		const uint32_t playerId = result->getU32("player_id");
		const uint16_t amount = result->getU16("amount");
		if (result->getU16("sale") == 1) {
			const ItemType& itemType = Item::items[result->getU16("itemtype")];
			if (itemType.id == 0) {
				continue;
			}

			Player* player = g_game().getPlayerByGUID(playerId);
			if (!player) {
				player = new Player(nullptr);
				if (!IOLoginData::loadPlayerById(player, playerId)) {
					delete player;
					continue;
				}
			}

			if (itemType.stackable) {
				uint16_t tmpAmount = amount;
				while (tmpAmount > 0) {
					uint16_t stackCount = std::min<uint16_t>(100, tmpAmount);
					if (Item* item = Item::CreateItem(itemType.id, stackCount); g_game().internalAddItem(player->getInbox(), item, INDEX_WHEREEVER, FLAG_NOLIMIT) != RETURNVALUE_NOERROR) {
						delete item;
						break;
					}

					tmpAmount -= stackCount;
				}
			} else {
				int32_t subType;
				if (itemType.charges != 0) {
					subType = itemType.charges;
				} else {
					subType = -1;
				}

				for (uint16_t i = 0; i < amount; ++i) {
					Item* item = Item::CreateItem(itemType.id, subType);
					if (g_game().internalAddItem(player->getInbox(), item, INDEX_WHEREEVER, FLAG_NOLIMIT) != RETURNVALUE_NOERROR) {
						delete item;
						break;
					}
				}
			}

			if (player->isOffline()) {
				IOLoginData::savePlayer(player);
				delete player;
			}
		} else {
			uint64_t totalPrice = result->getU64("price") * amount;

			Player* player = g_game().getPlayerByGUID(playerId);
			if (player) {
				player->setBankBalance(player->getBankBalance() + totalPrice);
			} else {
				IOLoginData::increaseBankBalance(playerId, totalPrice);
			}
		}
	} while (result->next());
}

void IOMarket::checkExpiredOffers()
{
	const time_t lastExpireDate = Game::getTimeNow() - g_configManager().getNumber(MARKET_OFFER_DURATION);

	std::ostringstream query;
	query << "SELECT `id`, `amount`, `price`, `itemtype`, `player_id`, `sale` FROM `market_offers` WHERE `created` <= " << lastExpireDate;
	g_databaseTasks().addTask(query.str(), IOMarket::processExpiredOffers, true);

	int32_t checkExpiredMarketOffersEachMinutes = g_configManager().getNumber(CHECK_EXPIRED_MARKET_OFFERS_EACH_MINUTES);
	if (checkExpiredMarketOffersEachMinutes <= 0) {
		return;
	}

	g_scheduler().addEvent(createSchedulerTask(checkExpiredMarketOffersEachMinutes * 60 * 1000, IOMarket::checkExpiredOffers));
}

uint32_t IOMarket::getPlayerOfferCount(uint32_t playerId)
{
	std::ostringstream query;
	query << "SELECT COUNT(*) AS `count` FROM `market_offers` WHERE `player_id` = " << playerId;

	DBResult_ptr result = Database::getInstance().storeQuery(query.str());
	if (!result) {
		return 0;
	}
	return result->get32("count");
}

MarketOfferEx IOMarket::getOfferByCounter(uint32_t timestamp, uint16_t counter)
{
	MarketOfferEx offer;

	const int32_t created = timestamp - g_configManager().getNumber(MARKET_OFFER_DURATION);

	std::ostringstream query;
	query << "SELECT `id`, `sale`, `itemtype`, `amount`, `created`, `price`, `player_id`, `anonymous`, (SELECT `name` FROM `players` WHERE `id` = `player_id`) AS `player_name` FROM `market_offers` WHERE `created` = " << created << " AND (`id` & 65535) = " << counter << " LIMIT 1";

	DBResult_ptr result = Database::getInstance().storeQuery(query.str());
	if (!result) {
		offer.id = 0;
		return offer;
	}

	offer.id = result->getU32("id");
	offer.type = static_cast<MarketAction_t>(result->getU16("sale"));
	offer.amount = result->getU16("amount");
	offer.counter = result->getU32("id") & 0xFFFF;
	offer.timestamp = result->getU32("created");
	offer.price = result->getU32("price");
	offer.itemId = result->getU16("itemtype");
	offer.playerId = result->getU32("player_id");
	if (result->getU16("anonymous") == 0) {
		offer.playerName = result->getString("player_name");
	} else {
		offer.playerName = "Anonymous";
	}
	return offer;
}

void IOMarket::createOffer(uint32_t playerId, MarketAction_t action, uint32_t itemId, uint16_t amount, uint32_t price, bool anonymous)
{
	std::ostringstream query;
	query << "INSERT INTO `market_offers` (`player_id`, `sale`, `itemtype`, `amount`, `price`, `created`, `anonymous`) VALUES (" << playerId << ',' << action << ',' << itemId << ',' << amount << ',' << price << ',' << Game::getTimeNow() << ',' << anonymous << ')';
	Database::getInstance().executeQuery(query.str());
}

void IOMarket::acceptOffer(uint32_t offerId, uint16_t amount)
{
	std::ostringstream query;
	query << "UPDATE `market_offers` SET `amount` = `amount` - " << amount << " WHERE `id` = " << offerId;
	Database::getInstance().executeQuery(query.str());
}

void IOMarket::deleteOffer(uint32_t offerId)
{
	std::ostringstream query;
	query << "DELETE FROM `market_offers` WHERE `id` = " << offerId;
	Database::getInstance().executeQuery(query.str());
}

void IOMarket::appendHistory(uint32_t playerId, MarketAction_t type, uint16_t itemId, uint16_t amount, uint32_t price, time_t timestamp, MarketOfferState_t state)
{
	std::ostringstream query;
	query << "INSERT INTO `market_history` (`player_id`, `sale`, `itemtype`, `amount`, `price`, `expires_at`, `inserted`, `state`) VALUES ("
		<< playerId << ',' << type << ',' << itemId << ',' << amount << ',' << price << ','
		<< timestamp << ',' << Game::getTimeNow() << ',' << state << ')';
	g_databaseTasks().addTask(query.str());
}

bool IOMarket::moveOfferToHistory(uint32_t offerId, MarketOfferState_t state)
{
	Database& db = Database::getInstance();

	std::ostringstream query;
	query << "SELECT `player_id`, `sale`, `itemtype`, `amount`, `price`, `created` FROM `market_offers` WHERE `id` = " << offerId;

	DBResult_ptr result = db.storeQuery(query.str());
	if (!result) {
		return false;
	}

	query.str(std::string());
	query << "DELETE FROM `market_offers` WHERE `id` = " << offerId;
	if (!db.executeQuery(query.str())) {
		return false;
	}

	appendHistory(result->getU32("player_id"), static_cast<MarketAction_t>(result->getU16("sale")), result->getU16("itemtype"), result->getU16("amount"), result->getU32("price"), Game::getTimeNow(), state);
	return true;
}

void IOMarket::updateStatistics()
{
	std::ostringstream query;
	query << "SELECT `sale` AS `sale`, `itemtype` AS `itemtype`, COUNT(`price`) AS `num`, MIN(`price`) AS `min`, MAX(`price`) AS `max`, SUM(`price`) AS `sum` FROM `market_history` WHERE `state` = " << OFFERSTATE_ACCEPTED << " GROUP BY `itemtype`, `sale`";
	DBResult_ptr result = Database::getInstance().storeQuery(query.str());
	if (!result) {
		return;
	}

	do {
		MarketStatistics* statistics;
		if (result->getU16("sale") == MARKETACTION_BUY) {
			statistics = &purchaseStatistics[result->getU16("itemtype")];
		} else {
			statistics = &saleStatistics[result->getU16("itemtype")];
		}

		statistics->numTransactions = result->getU32("num");
		statistics->lowestPrice = result->getU32("min");
		statistics->totalPrice = result->getU64("sum");
		statistics->highestPrice = result->getU32("max");
	} while (result->next());
}

MarketStatistics* IOMarket::getPurchaseStatistics(uint16_t itemId)
{
	auto it = purchaseStatistics.find(itemId);
	if (it == purchaseStatistics.end()) {
		return nullptr;
	}
	return &it->second;
}

MarketStatistics* IOMarket::getSaleStatistics(uint16_t itemId)
{
	auto it = saleStatistics.find(itemId);
	if (it == saleStatistics.end()) {
		return nullptr;
	}
	return &it->second;
}
