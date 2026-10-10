#include "kenshi.h"

#include <mutex>
#include <unordered_set>

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <tuple>
#include <unordered_map>

namespace kenshi {

const FunctionSig kFunctions[FnCount] = {
    {"GameWorld::mainLoop_GPUSensitiveStuff", 0x788A00, {0x48, 0x8B, 0xC4, 0x56, 0x57, 0x41, 0x54, 0x48, 0x83, 0xEC, 0x70, 0x48}},
    {"PlayerInterface::playerMove", 0x7FA850, {0x40, 0x57, 0x41, 0x55, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x50, 0x48, 0x83}},
    {"PlayerInterface::addOrderSelectedCharacters", 0x7F9E20, {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x48, 0x08, 0x55, 0x41, 0x56, 0x48, 0x8D}},
    {"PlayerInterface::newPlayerTaskSelectedCharacters", 0x7FA650, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x57}},
    {"PlayerInterface::setOrderSelectedCharacters", 0x7F3880, {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48}},
    {"PlayerInterface::stopCharactersMovement", 0x7F6470, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x83, 0xB9, 0x28, 0x02, 0x00}},
    {"Character::playerMoveOrderDefault", 0x5D22B0, {0x48, 0x89, 0x6C, 0x24, 0x20, 0x56, 0x41, 0x54, 0x41, 0x56, 0x48, 0x83}},
    {"AI::update4Frame", 0x5965B0, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x83, 0x79, 0x10, 0x00, 0x0F}},
    {"AI::periodicUpdate", 0x5112B0, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89}},
    {"GameWorld::setFrameSpeedMultiplier", 0x787CB0, {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48}},
    {"GameWorld::userPause", 0x787FB0, {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89}},
    {"HandleTable::resolve", 0x2676E0, {0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B}},
    {"GameWorld::togglePause", 0x787D40, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48}},
    {"MedicalSystem::applyDamage", 0x64F300, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89}},
    {"MedicalSystem::knockout", 0x644980, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x40, 0xF3, 0x0F, 0x10, 0x05, 0xA6, 0xF4}},
    {"Character::declareDead", 0x7A6200, {0x48, 0x8B, 0xC4, 0x55, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x48}},
    {"SaveManager::getSingleton", 0x37DD00, {0x48, 0x83, 0xEC, 0x38, 0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE, 0xFF, 0xFF}},
    {"SaveManager::save", 0x47B920, {0x40, 0x57, 0x48, 0x81, 0xEC, 0x00, 0x01, 0x00, 0x00, 0x48, 0xC7, 0x44}},
    {"SaveManager::load", 0x47B480, {0x4C, 0x8B, 0xDC, 0x57, 0x48, 0x81, 0xEC, 0x70, 0x01, 0x00, 0x00, 0x48}},
    {"RootObjectFactory::createRandomCharacter", 0x5836E0, {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41}},
    {"GameWorld::destroy(RootObject*)", 0x799AF0, {0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57}},
    {"Character::endCombatMode", 0x5C91C0, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x8D, 0x05, 0x03, 0xC1, 0x0B}},
    {"Character::ragdollMode", 0x5CBD60, {0x45, 0x85, 0xC0, 0x0F, 0x84, 0x13, 0x02, 0x00, 0x00, 0x44, 0x89, 0x44}},
    {"WeatherRegion::updateBT", 0x9DDE50, {0x40, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x83, 0x79, 0x38, 0x00, 0x48}},
    {"Season::getNewWeather", 0x9DD980, {0x48, 0x8B, 0xC4, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41}},
    {"WeatherInstance::setupWeather", 0x9DCF60, {0x4C, 0x8B, 0xDC, 0x57, 0x48, 0x81, 0xEC, 0x90, 0x00, 0x00, 0x00, 0x48}},
    {"RootObjectFactory::createItem", 0x580750, {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41}},
    {"ForgottenGUI::showTradeWindow", 0x791830, {0x44, 0x89, 0x49, 0x58, 0x8B, 0x42, 0x08, 0x89, 0x41, 0x68, 0x8B, 0x42}},
    {"Character::isRagdoll", 0x7D1440, {0x48, 0x83, 0xEC, 0x28, 0x80, 0xB9, 0xD4, 0x03, 0x00, 0x00, 0x00, 0x74}},
    {"PlayerInterface::recruit", 0x692820, {0x4C, 0x8B, 0xDC, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57}},
    {"PlayerInterface::addTaskNearestSelectedCharacter", 0x7FAE70, {0x40, 0x53, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0xC8}},
    {"PlayerInterface::addJobSelectedCharacters", 0x7F5A90, {0x40, 0x53, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x48, 0x8B}},
    {"EffectHandler::EffectHandler", 0x1034F0, {0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x48, 0x83, 0xEC, 0x70}},
    {"EffectHandler::affectObjects", 0x1020E0, {0x48, 0x8B, 0xC4, 0x57, 0x41, 0x54, 0x41, 0x55, 0x48, 0x81, 0xEC, 0x90}},
    {"EffectHandler::stop", 0x100B00, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0xC7, 0x41, 0x58}},
    {"WeatherRegion::updateWeatherEffects", 0x9DCAF0, {0x48, 0x89, 0x4C, 0x24, 0x08, 0x53, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41}},
    {"AnimationClass::startCombatAnimation", 0x5B7800, {0x40, 0x53, 0x55, 0x57, 0x41, 0x54, 0x48, 0x81, 0xEC, 0xC8, 0x00, 0x00}},
    {"AnimationClass::runCombatAnimation", 0x5B7600, {0x40, 0x53, 0x55, 0x57, 0x41, 0x54, 0x48, 0x81, 0xEC, 0xC8, 0x00, 0x00}},
    {"AnimationClass::endCombatAnimation", 0x5B3C60, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x83, 0xB9, 0xA8, 0x00, 0x00}},
    {"AnimationClass::playAction(AnimationData*)", 0x51EAB0, {0x48, 0x85, 0xD2, 0x0F, 0x84, 0xBA, 0x01, 0x00, 0x00, 0x48, 0x89, 0x5C}},
    {"AnimationClass::stopAction()", 0x51DFA0, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x60, 0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE}},
    {"AnimationClass::stopAction(name)", 0x51E0E0, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B}},
    {"AnimationClass::startStumble", 0x520490, {0x48, 0x85, 0xD2, 0x74, 0x62, 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48}},
    {"AnimationClass::endStumble", 0x51E840, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x81, 0x28, 0x02, 0x00}},
    {"AnimationClass::setCombatMode", 0x51CC30, {0x84, 0xD2, 0x75, 0x0C, 0x83, 0xB9, 0x44, 0x02, 0x00, 0x00, 0x00, 0x0F}},
    {"AnimationClass::setCarryMode", 0x51C8D0, {0xC7, 0x81, 0x40, 0x02, 0x00, 0x00, 0x00, 0x00, 0x80, 0xBF, 0x44, 0x88}},
    {"CharacterHuman::drawWeapon", 0x5DBF80, {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x48}},
    {"CharacterHuman::sheatheWeapon", 0x5CC820, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x60, 0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE}},
    {"AnimationClass::setCombatModeLegsIdle", 0x51C920, {0x38, 0x91, 0x4C, 0x02, 0x00, 0x00, 0x88, 0x91, 0x4C, 0x02, 0x00, 0x00}},
    {"AnimationClass::setCombatModeUpperIdle", 0x51C940, {0x38, 0x91, 0x4D, 0x02, 0x00, 0x00, 0x88, 0x91, 0x4D, 0x02, 0x00, 0x00}},
    {"SingleAnimation::update", 0x5B1700, {0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x50, 0x0F, 0x29}},
    {"AnimationClass::runAnimation(AnimationData*, layer)", 0x5B7AC0, {0x48, 0x89, 0x6C, 0x24, 0x20, 0x56, 0x48, 0x83, 0xEC, 0x70, 0x48, 0x83}},
    {"InventorySection::canItemGoHere", 0x74BE40, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89}},
    {"InventorySection::getValidInventoryPosition", 0x74BEC0, {0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x20}},
    {"InventorySection::existsItemInFootprint", 0x7466F0, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89}},
    {"PlayerInterface::pickupItem", 0x7FB3A0, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x8B}},
    {"Character::giveItem", 0x5CB400, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89}},
    {"CharacterHuman::dropItem", 0x5CA740, {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48}},
    {"ForgottenGUI::createScreenLabel", 0x73FAF0, {0x40, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x48, 0xC7, 0x44, 0x24, 0x30, 0xFE}},
    {"ScreenLabel::setTracking", 0x6E25F0, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0x42, 0x08, 0x48, 0x8B, 0xD9}},
    {"ScreenLabel::setColor", 0x6E2670, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48}},
    {"MedicalSystem::reassessCollapseMode", 0x649320, {0x48, 0x8B, 0xC4, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57}},
    {"AnimationClass::animationSelection", 0x520500, {0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x41, 0x54}},
    {"CharMovement::trackAnimationMovement", 0x65E240, {0x48, 0x83, 0xEC, 0x28, 0x38, 0x91, 0x7C, 0x03, 0x00, 0x00, 0x74, 0x17}},
    {"CharMovement::combatMovementUpdate", 0x2AF1E0, {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x10, 0x48}},
    {"increaseStat", 0x8C5DF0, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0xF3, 0x0F, 0x10, 0x05, 0x0A, 0x65}},
    {"PlayerInterface::objectSelected", 0x7F7F20, {0x48, 0x85, 0xD2, 0x0F, 0x84, 0xD6, 0x04, 0x00, 0x00, 0x53, 0x57, 0x41}},
    {"PlayerInterface::unselectAll", 0x7F8DA0, {0x40, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x83, 0xB9, 0xA0, 0x02, 0x00}},
    {"Dialogue::say", 0x67FD80, {0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x48}},
    {"Dialogue::setInDialog", 0x6746A0, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B}},
    {"Dialogue::setResponesGUI", 0x674070, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x8B, 0x0D}},
    {"Dialogue::setConversationReplyGUI", 0x674170, {0x40, 0x57, 0x48, 0x83, 0xEC, 0x60, 0x48, 0xC7, 0x44, 0x24, 0x28, 0xFE}},
    {"Dialogue::replyClicked", 0x683DF0, {0x48, 0x8B, 0xC4, 0x56, 0x57, 0x41, 0x54, 0x48, 0x83, 0xEC, 0x60, 0x48}},
    {"Dialogue::sendEvent", 0x684990, {0x44, 0x89, 0x44, 0x24, 0x18, 0x53, 0x55, 0x57, 0x41, 0x55, 0x48, 0x83}},
    {"Dialogue::sendEventOverride", 0x685660, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89}},
    {"Dialogue::startConversation", 0x683F90, {0x40, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x38, 0x45}},
    {"Dialogue::startPlayerConversation", 0x684320, {0x48, 0x8B, 0xC4, 0x57, 0x41, 0x54, 0x41, 0x55, 0x48, 0x83, 0xEC, 0x70}},
    {"Dialogue::_doActions", 0x680560, {0x48, 0x8B, 0xC4, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56}},
    {"AITaskSytem::update", 0x50D920, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x80, 0xB9, 0x6D, 0x02, 0x00, 0x00}},
    {"SensoryData::dialogAssessmentUpdate", 0x85A5F0, {0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x6C, 0x24, 0x20, 0x57, 0x48}},
    {"SensoryData::assessCrimes", 0x854D10, {0x40, 0x53, 0x55, 0x56, 0x41, 0x55, 0x48, 0x83, 0xEC, 0x28, 0x4C, 0x8D}},
    {"Blackboard::update", 0x26A320, {0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57}},
    {"Blackboard::periodicUpdate", 0x2732B0, {0x40, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x0F, 0x29, 0x74, 0x24, 0x20, 0x48}},
    {"FactionWarMgr::periodicUpdate", 0x9CB310, {0x48, 0x8B, 0xC4, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57}},
    {"FactionUniqueSquadManager::periodicUpdate", 0x2DD680, {0x48, 0x83, 0xEC, 0x18, 0x48, 0xC7, 0x04, 0x24, 0xFE, 0xFF, 0xFF, 0xFF}},
    {"FactionRelations::affectRelations(amount)", 0x6B2EA0, {0x48, 0x85, 0xD2, 0x0F, 0x84, 0xDF, 0x00, 0x00, 0x00, 0x48, 0x89, 0x5C}},
    {"FactionRelations::affectRelations(event)", 0x6B2D20, {0x48, 0x85, 0xD2, 0x0F, 0x84, 0x2E, 0x01, 0x00, 0x00, 0x48, 0x89, 0x5C}},
    {"FactionRelations::setRelation", 0x6B4D80, {0x48, 0x89, 0x54, 0x24, 0x10, 0x48, 0x83, 0xEC, 0x38, 0x48, 0x8D, 0x54}},
    {"BountyManager::setCrime", 0x852C80, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89}},
    {"BountyManager::assignBountyForCrimes", 0x853EC0, {0x40, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xF9, 0xE8, 0x4B, 0xA9}},
    {"PlayerInterface::focusCameraSelectedCharacter", 0x7F37F0, {0x48, 0x89, 0x5C, 0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B}},
    {"ActivePlatoon::addCharacterAt", 0x796620, {0x48, 0x85, 0xD2, 0x0F, 0x84, 0x1C, 0x04, 0x00, 0x00, 0x48, 0x8B, 0xC4}},
    {"PlayerInterface::createSquad", 0x7F4910, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0x05, 0x73, 0xFD, 0x93}},
    {"ForgottenGUI::closeCharacterEditor", 0x6E22B0, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x8B, 0x89}},
    {"ForgottenGUI::showCharacterEditor", 0x6E32F0, {0x48, 0x89, 0x54, 0x24, 0x10, 0x56, 0x57, 0x41, 0x54, 0x48, 0x83, 0xEC}},
    {"Character::setAppearanceData", 0x5B9E90, {0x48, 0x8B, 0xC4, 0x55, 0x57, 0x41, 0x54, 0x48, 0x8D, 0x68, 0xA1, 0x48}},
    {"GameData bool map []", 0x6C7C0, {0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x48, 0x81, 0xEC, 0x90}},
    {"GameData string map []", 0x6D190, {0x48, 0x8B, 0xC4, 0x56, 0x57, 0x41, 0x54, 0x48, 0x81, 0xEC, 0x90, 0x00}},
    {"GameData int map []", 0x6CF30, {0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x48, 0x81, 0xEC, 0x90}},
    {"GameData float map []", 0xB0AB0, {0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x48, 0x81, 0xEC, 0x90}},
    {"GameData vec3 map []", 0xB0D40, {0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x48, 0x81, 0xEC, 0x90}},
    {"GameData quat map []", 0x2E6110, {0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x48, 0x81, 0xEC, 0x90}},
    {"std::string::assign", 0x69BC0, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89}},
    {"Character::addOrder", 0x5D20D0, {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x41}},
    {"Character::addJob", 0x5C8DA0, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89}},
    {"Character::setStandingOrder", 0x5CAA50, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48}},
    {"Character::pickupObject", 0x5CFF90, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x80, 0xB9}},
    {"Character::dropCarriedObject", 0x5CE1E0, {0x48, 0x8B, 0xC4, 0x57, 0x41, 0x54, 0x41, 0x55, 0x48, 0x83, 0xEC, 0x70}},
    {"PlayerInterface::setCurrentPlatoon", 0x7F2800, {0x48, 0x83, 0xEC, 0x28, 0x48, 0x39, 0x91, 0xA8, 0x02, 0x00, 0x00, 0x74}},
    {"SaveManager::showLoad", 0x4824C0, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE}},
    {"Character::reThinkCurrentAIAction", 0x5C8330, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x81, 0x48, 0x06, 0x00}},
    {"Character::getOwnerships", 0x7956A0, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE}},
    {"BuildingInterior shop furniture", 0x54ACB0, {0x40, 0x53, 0x55, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x83, 0xB9, 0x08}},
    {"InventoryGUI::getNPCTrader", 0x70E2D0, {0x48, 0x83, 0xEC, 0x28, 0x48, 0x83, 0x3D, 0x14, 0x49, 0xA2, 0x01, 0x02}},
    {"Character::takeMoney", 0x7965F0, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0xDA, 0xE8, 0xB6, 0x56, 0x8B}},
    {"InventoryGUI::RClickAutoTrade", 0x713D20, {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41}},
    // ---- lot B: factions
    {"FactionRelations::getRelationData", 0x6B4C60, {0x48, 0x89, 0x54, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x4C, 0x8B}},
    {"BountyManager bounties operator[]", 0x5E7EE0, {0x40, 0x56, 0x57, 0x41, 0x54, 0x48, 0x83, 0xEC, 0x40, 0x48, 0xC7, 0x44}},
    // ---- lot A: doors
    {"DoorStuff::openDoor", 0x297040, {0x48, 0x83, 0xEC, 0x48, 0x83, 0xB9, 0x80, 0x03, 0x00, 0x00, 0x00, 0x75}},
    {"DoorStuff::closeDoor", 0x298C50, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x8B, 0x01, 0x48, 0x8B, 0xD9}},
    {"DoorStuff::lockDoor", 0x2969F0, {0x83, 0xB9, 0x80, 0x03, 0x00, 0x00, 0x00, 0x75, 0x1C, 0xF3, 0x0F, 0x10}},
    {"DoorStuff::unlockDoor", 0x56A3D0, {0x48, 0x8B, 0x81, 0x70, 0x03, 0x00, 0x00, 0xC6, 0x40, 0x20, 0x00, 0xE9}},
    {"DoorStuff::openButton", 0x546FB0, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x40, 0x33, 0xC0, 0x48, 0x8B, 0xD9, 0x48}},
    {"DoorStuff::lockButton", 0x547060, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x80, 0xB9}},
    // ---- end lot A
    // ---- lot D: prisons
    {"Character::setPrisonMode", 0x330600, {0x48, 0x8B, 0xC4, 0x55, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x48}},
    {"Character::setChainedMode", 0x32E590, {0x40, 0x55, 0x56, 0x57, 0x48, 0x81, 0xEC, 0xB0, 0x00, 0x00, 0x00, 0x48}},
    {"StateBroadcastData::setSlaveState", 0x5A4940, {0x48, 0x8B, 0xC4, 0x57, 0x48, 0x81, 0xEC, 0xA0, 0x02, 0x00, 0x00, 0x48}},
    // ---- lot C: ranged
    {"GunClass::shoot", 0x43A730, {0x48, 0x8B, 0xC4, 0x57, 0x41, 0x54, 0x41, 0x55, 0x48, 0x81, 0xEC, 0xB0}},
    {"projectile pool get", 0x43A2F0, {0x4C, 0x8B, 0xDC, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x48}},
    // ---- lot E: buildings
    {"<PreviewGroup>::createBuildings", 0x4D72A0, {0x48, 0x8B, 0xC4, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57}},
    {"RootObjectFactory::createBuilding", 0x57CC70, {0x48, 0x8B, 0xC4, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56}},
    {"Building::buyMeCallback", 0x7AD6C0, {0x83, 0xFA, 0x02, 0x0F, 0x85, 0x8A, 0x07, 0x00, 0x00, 0x48, 0x8B, 0xC4}},
    {"Building::confirmDismantle", 0x54FEA0, {0x83, 0xFA, 0x02, 0x0F, 0x85, 0x73, 0x01, 0x00, 0x00, 0x57, 0x48, 0x83}},
    {"Building::addConstructionProgress", 0x5595A0, {0x48, 0x8B, 0xC4, 0x55, 0x41, 0x54, 0x41, 0x55, 0x48, 0x8D, 0xA8, 0xE8}},
    {"Building::addDismantleProgress", 0x2A2860, {0x48, 0x8B, 0xC4, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57}},
    {"Building::clearUsageNodes", 0x54C4D0, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89}},
    {"Building::calculateSaleValue", 0x7AD300, {0x40, 0x53, 0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00, 0x48, 0xC7, 0x44}},
    // ---- admin console
    {"Character::healCompletely", 0x6464C0, {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48}},
    {"ForgottenGUI::showInventoryBuilding", 0x6E6640, {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89}},
    // ---- fix G5
    {"Character::removePermajob", 0x5C9000, {0x48, 0x8B, 0x89, 0x50, 0x06, 0x00, 0x00, 0x48, 0x8B, 0x49, 0x20, 0xE9}},
    {"Character::movePermajob", 0x5C8FC0, {0x48, 0x8B, 0x89, 0x50, 0x06, 0x00, 0x00, 0x48, 0x8B, 0x49, 0x20, 0xE9}},
    {"Character::removeJob", 0x5C8EB0, {0x48, 0x8B, 0x89, 0x50, 0x06, 0x00, 0x00, 0x48, 0x8B, 0x49, 0x20, 0xE9}},
    {"Character::getPermajob", 0x5C8EF0, {0x48, 0x8B, 0x81, 0x50, 0x06, 0x00, 0x00, 0x48, 0x8B, 0x48, 0x20, 0xE9}},
    {"Character::getPermajobCount", 0x5C8F30, {0x48, 0x8B, 0x81, 0x50, 0x06, 0x00, 0x00, 0x48, 0x8B, 0x48, 0x20, 0xE9}},
    {"CharMovement::_setPositionAndTeleport", 0x65E940, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48}},
    // ---- placement validity (each one is called by PreviewBuilding's checks: see docs/MOTEUR.md section 10)
    {"UtilityT::getTerrainHeight", 0x9B3710, {0x45, 0x33, 0xC0, 0xE9, 0xE8, 0xE0, 0x66, 0xFF, 0xCC, 0xCC, 0xCC, 0xCC}},
    {"UtilityT::getTerrainWithWaterHeight", 0x9B3720, {0x48, 0x83, 0xEC, 0x38, 0x0F, 0x29, 0x74, 0x24, 0x20, 0xF3, 0x0F, 0x10}},
    {"UtilityT::isIndoors", 0x9B2BA0, {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x57, 0x48, 0x81, 0xEC, 0x20}},
    {"TownList::getNearestTown", 0x927F10, {0x48, 0x8B, 0xC4, 0x4C, 0x89, 0x48, 0x20, 0x48, 0x89, 0x50, 0x10, 0x55}},
    {"TownBase::withinBordersRange", 0x926D50, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x83, 0xB9}},
    {"TownList::getNearestWithinItsRadius", 0x928890, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55}},
    // ---- diplomacy
    {"UniqueNPCManager map operator[] (UniqueMapIndex)", 0x349950, {0x40, 0x57, 0x41, 0x54, 0x41, 0x55, 0x48, 0x83, 0xEC, 0x40, 0x48, 0xC7}},
    {"TownBase::setOverride", 0x9FE7A0, {0x40, 0x55, 0x56, 0x57, 0x48, 0x8B, 0xEC, 0x48, 0x83, 0xEC, 0x60, 0x48}},
    {"TownBase::setFaction", 0x9287D0, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B}},
    // ---- map markers
    {"MapScreen::worldToMapCoords", 0x48C3E0, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B}},
    {"MapScreen::getMarkerColor", 0x48F320, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x85, 0xC9}},
    {"FactionWarMgr::getCurrentCampaign", 0x283500, {0x40, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x4C, 0x8B, 0x41, 0x30, 0x48, 0x8B}},
    {"PortraitMainCellView::update", 0x415150, {0x48, 0x8B, 0xC4, 0x56, 0x57, 0x41, 0x54, 0x48, 0x83, 0xEC, 0x70, 0x48}},
    {"PortraitMainCellView::~PortraitMainCellView", 0x426450, {0x48, 0x89, 0x4C, 0x24, 0x08, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x48, 0xC7}},
    {"MainBarGUI::updateCurrentPlatoon", 0x415880, {0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41}},
    // ---- ground drops
    {"Inventory::dropItem", 0x745D90, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x83}},
    {"CharacterAnimal::dropItem", 0x5CA4A0, {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48}},
    // ---- crash report (main loop's catch(...) funclets 0x1373AB0 / 0x1373A20 call it)
    {"writeCrashDump", 0x744D20, {0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x48, 0x8D, 0xAC, 0x24}},
    // ---- conversations (KenshiLib 1.0.65 0x6740B0 + 0x780, like setInDialog / replyClicked; body read: DT_END_DIALOG off the
    //      main thread, else _hasEnded = 1 and setInDialog(false) at 0x674B46)
    {"Dialogue::endDialogue", 0x674830, {0x40, 0x53, 0x56, 0x41, 0x54, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0xF1}},
    // ---- workshop (located by their strings: "Research complete: {1}", "Used artifacts for '{1}' research.",
    //      "Failed to learn from blueprint...", "_removeCraft"; and by their callers, see docs/MOTEUR.md)
    {"Research::startResearch", 0x8348A0, {0x4C, 0x8B, 0xDC, 0x56, 0x57, 0x41, 0x54, 0x48, 0x81, 0xEC, 0x90, 0x00}},
    {"Research::stopResearch", 0x830DE0, {0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57}},
    {"Research::completeResearch", 0x834550, {0x48, 0x8B, 0xC4, 0x55, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x48}},
    {"Research::payCosts", 0x8343C0, {0x40, 0x55, 0x56, 0x57, 0x48, 0x81, 0xEC, 0x80, 0x02, 0x00, 0x00, 0x48}},
    {"Research::progressResearch", 0x836B70, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x70, 0x48, 0x8B, 0xD9, 0x8B, 0x89, 0x74}},
    {"Research::isInQueue", 0x82EAA0, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89}},
    {"Item::learnResearch", 0x2B65D0, {0x40, 0x55, 0x48, 0x8D, 0x6C, 0x24, 0xA9, 0x48, 0x81, 0xEC, 0x00, 0x01}},
    {"CraftingBuilding::_addCraft", 0x2B5CA0, {0x48, 0x8B, 0xC4, 0x53, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41}},
    {"CraftingBuilding::_removeCraft", 0x2B6A20, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x81, 0xEC, 0x80, 0x00, 0x00}},
    {"CraftingQueue::addCraftButton", 0x2B9770, {0x4C, 0x8B, 0xDC, 0x55, 0x56, 0x57, 0x48, 0x81, 0xEC, 0xB0, 0x00, 0x00}},
    {"CraftingQueue::removeCraftButton", 0x2B9940, {0x4C, 0x8B, 0xDC, 0x57, 0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00, 0x48}},
    {"CraftingQueue::craftRemoved", 0x2B9A50, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x83, 0x79}},
    {"CraftingQueue::repeatButton", 0x2B5000, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x83, 0x79, 0x68, 0x00, 0x48, 0x8D}},
    {"UseableStuff::stopOperating", 0x2ACA90, {0x48, 0x81, 0xC1, 0xD0, 0x03, 0x00, 0x00, 0xE9, 0x73, 0xBA, 0xD8, 0xFF}},
    {"std::set<hand>::insert", 0xF7D90, {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89}},
    {"Town::updatePowerGrid", 0x92CD60, {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x55, 0x56, 0x57, 0x41, 0x55}},
    {"UseableStuff::togglePowerButton", 0x297A60, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x40, 0x80, 0xB9, 0xB5, 0x03, 0x00, 0x00}},
    {"UseableStuff::toggleBattButton", 0x297AE0, {0x48, 0x83, 0xEC, 0x48, 0x80, 0xB9, 0xB4, 0x03, 0x00, 0x00, 0x00, 0x0F}},
    {"ManagementScreen::refreshResearchList", 0x49A4C0, {0x48, 0x8B, 0xC4, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57}},
    {"Research::checkRequirements", 0x832FA0, {0x40, 0x55, 0x56, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0xB9, 0x48, 0x81, 0xEC}},
    // ---- squad window
    {"ActivePlatoon::swapCharacters", 0x792E60, {0x41, 0x3B, 0xD0, 0x0F, 0x84, 0x18, 0x01, 0x00, 0x00, 0x44, 0x89, 0x44}},
    {"Faction::changePlatoonIndex", 0x7F3440, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x4C, 0x8B}},
    {"Faction::destroyPlatoon", 0x6BA9D0, {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89}},
    {"ActivePlatoon::setName", 0x4BE480, {0x48, 0x8B, 0x49, 0x78, 0x49, 0x83, 0xC9, 0xFF, 0x45, 0x33, 0xC0, 0x48}},
    {"Character::getPermajobData", 0x5C8F10, {0x48, 0x8B, 0x81, 0x50, 0x06, 0x00, 0x00, 0x48, 0x8B, 0x48, 0x20, 0xE9}},
    // ---- game clock
    {"GameClock::setHourOfDay", 0x66CF30, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x0F, 0x2E, 0x0D, 0x8F, 0xB4, 0x01}},
};

namespace {

uintptr_t g_base = 0;

// ---- SEH-guarded primitives. These functions hold no C++ objects with destructors (required
// for __try) and are the only places that touch raw game memory.

bool SafeCopy(void* dst, const void* src, size_t n) {
    __try {
        std::memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <class T>
bool Rd(const void* p, uintptr_t offset, T& out) {
    if (!p) return false;
    return SafeCopy(&out, reinterpret_cast<const uint8_t*>(p) + offset, sizeof(T));
}

uintptr_t Vtable(const void* obj) {
    uintptr_t vt = 0;
    return Rd(obj, 0, vt) ? vt : 0;
}

using VirtGetVec3 = void* (*)(void* self, float* ret);
bool CallGetVec3(void* fn, void* self, float* ret) {
    __try {
        reinterpret_cast<VirtGetVec3>(fn)(self, ret);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

using VirtBool = bool (*)(void* self);
bool CallBool(void* fn, void* self, bool& out) {
    __try {
        out = reinterpret_cast<VirtBool>(fn)(self);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

using VirtTeleport = void (*)(void* self, const float* pos, const float* quat);
bool CallTeleport(void* fn, void* self, const float* pos, const float* quat) {
    __try {
        reinterpret_cast<VirtTeleport>(fn)(self, pos, quat);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

using VirtSetDest = void (*)(void* self, const float* pos, int priority, bool notVertical);
bool CallSetDest(void* fn, void* self, const float* pos, int prio) {
    __try {
        reinterpret_cast<VirtSetDest>(fn)(self, pos, prio, false);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

using VirtVoid = void (*)(void* self);
bool CallVoid(void* fn, void* self) {
    __try {
        reinterpret_cast<VirtVoid>(fn)(self);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

using FnFloat = void (*)(void* self, float v);
bool CallFloat(void* fn, void* self, float v) {
    __try {
        reinterpret_cast<FnFloat>(fn)(self, v);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

using FnBoolArg = void (*)(void* self, bool v);
bool CallBoolArg(void* fn, void* self, bool v) {
    __try {
        reinterpret_cast<FnBoolArg>(fn)(self, v);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

using FnResolve = void* (*)(void* table, const void* hand, bool flag);
void* CallResolve(void* fn, void* table, const void* hand) {
    __try {
        return reinterpret_cast<FnResolve>(fn)(table, hand, true);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

using FnMedFloat = void (*)(void* med, float v);
bool CallMedFloat(void* fn, void* med, float v) {
    __try {
        reinterpret_cast<FnMedFloat>(fn)(med, v);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <class T>
bool Wr(void* p, uintptr_t offset, const T& v) {
    if (!p) return false;
    return SafeCopy(reinterpret_cast<uint8_t*>(p) + offset, &v, sizeof(T));
}

// Reads a boost::unordered_set of pointers (Kenshi's layout: see off::US_*).
void ReadPointerSet(const void* set, std::vector<void*>& out, size_t cap) {
    uint64_t size = 0, bucketCount = 0;
    void** buckets = nullptr;
    if (!Rd(set, off::US_size, size) || size == 0 || size > cap) return;
    if (!Rd(set, off::US_bucketCount, bucketCount) || !Rd(set, off::US_buckets, buckets) || !buckets || bucketCount > (1u << 24)) return;
    void* node = nullptr;
    if (!Rd(buckets, bucketCount * sizeof(void*), node)) return;
    for (uint64_t i = 0; node && i < size; ++i) {
        void* v = nullptr;
        if (!Rd(node, off::USNode_value, v)) return;
        out.push_back(v);
        if (!Rd(node, off::USNode_next, node)) return;
    }
}

// std::string as compiled by Visual Studio 2010 (Kenshi's compiler): 40 bytes,
// { char buf[16] | char* ptr; size_t size; size_t capacity; allocator (1 byte, padded) }.
// Strings shorter than 16 chars live inline, so building one never allocates, and the game
// never has to free memory owned by a different C runtime.
struct GameString {
    alignas(8) uint8_t raw[0x28] = {};
};
static_assert(sizeof(GameString) == 0x28, "VS2010 std::string is 40 bytes");
bool MakeGameString(const std::string& s, GameString& out) {
    if (s.size() > 15) return false;
    std::memset(out.raw, 0, sizeof(out.raw));
    std::memcpy(out.raw, s.data(), s.size());
    const uint64_t size = s.size(), cap = 15;
    std::memcpy(out.raw + 0x10, &size, 8);
    std::memcpy(out.raw + 0x18, &cap, 8);
    return true;
}
bool ReadGameString(const void* p, std::string& out) {
    uint64_t size = 0, cap = 0;
    if (!Rd(p, 0x10, size) || !Rd(p, 0x18, cap) || size > cap || cap > 4096) return false;
    const void* chars = p;
    if (cap >= 16 && !Rd(p, 0, chars)) return false;
    out.resize(size_t(size));
    return size == 0 || SafeCopy(out.data(), chars, size_t(size));
}

using FnThisPtr = void* (*)(void*);
void* CallNoArgPtrOn(void* fn, void* self) {
    __try {
        return reinterpret_cast<FnThisPtr>(fn)(self);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}
using FnNoArgPtr = void* (*)();
void* CallNoArgPtr(void* fn) {
    __try {
        return reinterpret_cast<FnNoArgPtr>(fn)();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}
using FnStrBool = void (*)(void* self, const void* str, bool b);
bool CallStrBool(void* fn, void* self, const void* str, bool b) {
    __try {
        reinterpret_cast<FnStrBool>(fn)(self, str, b);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
using FnStr = void (*)(void* self, const void* str);
bool CallStr(void* fn, void* self, const void* str) {
    __try {
        reinterpret_cast<FnStr>(fn)(self, str);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

using FnPtrArg2 = void (*)(void* self, void* arg);
bool CallPtrArg2(void* fn, void* self, void* arg) {
    __try {
        reinterpret_cast<FnPtrArg2>(fn)(self, arg);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* VSlot(const void* obj, uintptr_t slotOffset) {
    const uintptr_t vt = Vtable(obj);
    if (!vt) return nullptr;
    void* fn = nullptr;
    return Rd(reinterpret_cast<void*>(vt), slotOffset, fn) ? fn : nullptr;
}

bool Finite(const kc::Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

void* Medical(const Character* c) {
    return IsCharacter(c) ? reinterpret_cast<uint8_t*>(const_cast<Character*>(c)) + off::CH_medical : nullptr;
}

void* Movement(const Character* c) {
    void* m = nullptr;
    if (!IsCharacter(c) || !Rd(c, off::CH_movement, m) || !m) return nullptr;
    return Vtable(m) == Addr(rva::VtCharMovement) ? m : nullptr;
}

bool ReadHandle(const void* p, kc::Handle& h) {
    uint8_t raw[off::HandSize];
    if (!SafeCopy(raw, p, sizeof(raw))) return false;
    std::memcpy(&h.type, raw + off::H_type, 4);
    std::memcpy(&h.container, raw + off::H_container, 4);
    std::memcpy(&h.containerSerial, raw + off::H_containerSerial, 4);
    std::memcpy(&h.index, raw + off::H_index, 4);
    std::memcpy(&h.serial, raw + off::H_serial, 4);
    return true;
}

} // namespace

bool Init(std::string* err) {
    g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!g_base) { if (err) *err = "cannot find kenshi_x64.exe module"; return false; }
    for (const auto& f : kFunctions) {
        uint8_t live[sizeof(f.prologue)];
        if (!SafeCopy(live, reinterpret_cast<void*>(g_base + f.rva), sizeof(live)) ||
            std::memcmp(live, f.prologue, sizeof(live)) != 0) {
            if (err) *err = std::string("unexpected code at ") + f.name + " (game updated or another mod patched it)";
            return false;
        }
    }
    return true;
}

uintptr_t Base() { return g_base; }
uintptr_t Addr(uintptr_t r) { return g_base + r; }
bool InGameImage(const void* p) {
    static const uintptr_t size = [] {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew);
        return uintptr_t(nt->OptionalHeader.SizeOfImage);
    }();
    const auto a = reinterpret_cast<uintptr_t>(p);
    return g_base && a >= g_base && a < g_base + size;
}
void* FnAddr(Fn f) { return reinterpret_cast<void*>(g_base + kFunctions[f].rva); }

GameWorld* World() {
    auto* w = reinterpret_cast<GameWorld*>(Addr(rva::GameWorldInstance));
    return Vtable(w) == Addr(rva::VtGameWorld) ? w : nullptr;
}

PlayerInterface* Player() {
    GameWorld* w = World();
    void* p = nullptr;
    if (!w || !Rd(w, off::GW_player, p) || !p) return nullptr;
    return Vtable(p) == Addr(rva::VtPlayerInterface) ? static_cast<PlayerInterface*>(p) : nullptr;
}

bool IsCharacter(const void* obj) {
    if (!obj) return false;
    const uintptr_t vt = Vtable(obj);
    return vt && (vt == Addr(rva::VtCharacterHuman) || vt == Addr(rva::VtCharacter) || vt == Addr(rva::VtCharacterAnimal));
}

void PlayerCharacters(std::vector<Character*>& out) {
    out.clear();
    PlayerInterface* pi = Player();
    if (!pi) return;
    const auto* lk = reinterpret_cast<const uint8_t*>(pi) + off::PI_playerCharacters;
    uint32_t count = 0;
    Character** data = nullptr;
    if (!Rd(lk, off::LK_count, count) || !Rd(lk, off::LK_data, data) || !data || count > 4096) return;
    for (uint32_t i = 0; i < count; ++i) {
        Character* c = nullptr;
        if (Rd(data, i * sizeof(void*), c) && IsCharacter(c)) out.push_back(c);
    }
}

void SelectedHandles(std::vector<kc::Handle>& out) {
    out.clear();
    PlayerInterface* pi = Player();
    if (!pi) return;
    const auto* set = reinterpret_cast<const uint8_t*>(pi) + off::PI_selectedCharacters;
    uint64_t size = 0, bucketCount = 0;
    void** buckets = nullptr;
    if (!Rd(set, off::US_size, size) || size == 0 || size > 4096) return;
    if (!Rd(set, off::US_bucketCount, bucketCount) || !Rd(set, off::US_buckets, buckets) || !buckets || bucketCount > (1u << 24)) return;
    void* node = nullptr;
    if (!Rd(buckets, bucketCount * sizeof(void*), node)) return;
    for (uint64_t i = 0; node && i < size; ++i) {
        kc::Handle h;
        if (!ReadHandle(reinterpret_cast<uint8_t*>(node) + off::USNode_value, h)) return;
        out.push_back(h);
        if (!Rd(node, off::USNode_next, node)) return;
    }
}

void ActiveCharacters(std::vector<Character*>& out) {
    out.clear();
    GameWorld* w = World();
    if (!w) return;
    std::vector<void*> raw;
    ReadPointerSet(reinterpret_cast<const uint8_t*>(w) + off::GW_charUpdateList, raw, 65536);
    for (void* p : raw) if (IsCharacter(p)) out.push_back(static_cast<Character*>(p));
}

bool ObjectHandle(const void* rootObject, kc::Handle& out) {
    return rootObject && ReadHandle(reinterpret_cast<const uint8_t*>(rootObject) + off::RO_handle, out) && out.valid();
}

void* ResolveObject(const kc::Handle& h) {
    if (!h.valid()) return nullptr;
    if (Character* c = Resolve(h)) return c;
    return ResolveItem(h);
}

void MakeHand(const kc::Handle& h, void* out) {
    auto* hand = static_cast<uint8_t*>(out);
    std::memset(hand, 0, off::HandSize);
    const uintptr_t vt = Addr(rva::VtHand);
    std::memcpy(hand, &vt, 8);
    std::memcpy(hand + off::H_type, &h.type, 4);
    std::memcpy(hand + off::H_container, &h.container, 4);
    std::memcpy(hand + off::H_containerSerial, &h.containerSerial, 4);
    std::memcpy(hand + off::H_index, &h.index, 4);
    std::memcpy(hand + off::H_serial, &h.serial, 4);
}

bool HandleFromHand(const void* hand, kc::Handle& out) {
    return hand && ReadHandle(hand, out) && out.valid();
}

namespace {
constexpr int kTradeLooting = 2;   // TradeWindowType::TW_LOOTING
using FnShowTrade = void (*)(void* gui, const void* a, const void* b, int type);
bool CallShowTrade(void* fn, void* gui, const void* a, const void* b, int type) {
    __try {
        reinterpret_cast<FnShowTrade>(fn)(gui, a, b, type);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
} // namespace

bool OpenLootWindow(Character* looter, void* target) {
    if (!IsCharacter(looter) || !target) return false;
    // The game stores the request; its GUI opens the window on its next update.
    const auto* a = reinterpret_cast<const uint8_t*>(looter) + off::RO_handle;
    const auto* b = reinterpret_cast<const uint8_t*>(target) + off::RO_handle;
    return CallShowTrade(FnAddr(FnShowTradeWindow), reinterpret_cast<void*>(Addr(rva::TradeGui)), a, b, kTradeLooting);
}

void DeadBodies(std::vector<Character*>& out) {
    out.clear();
    GameWorld* w = World();
    if (!w) return;
    const auto* map = reinterpret_cast<const uint8_t*>(w) + off::GW_deathParade;
    uint64_t size = 0, bucketCount = 0;
    void** buckets = nullptr;
    if (!Rd(map, off::US_size, size) || size == 0 || size > 65536) return;
    if (!Rd(map, off::US_bucketCount, bucketCount) || !Rd(map, off::US_buckets, buckets) || !buckets || bucketCount > (1u << 24)) return;
    void* node = nullptr;
    if (!Rd(buckets, bucketCount * sizeof(void*), node)) return;
    for (uint64_t i = 0; node && i < size; ++i) {
        void* c = nullptr;
        if (Rd(node, off::HandMapNode_mapped, c) && IsCharacter(c)) out.push_back(static_cast<Character*>(c));
        if (!Rd(node, off::USNode_next, node)) break;
    }
}

Character* Resolve(const kc::Handle& h) {
    // Build a real `hand` (its vtable pointer is copied from a live one) and ask the game.
    static uintptr_t handVtable = 0;
    if (!handVtable) {
        std::vector<Character*> squad;
        PlayerCharacters(squad);
        if (squad.empty() || !Rd(squad.front(), off::RO_handle, handVtable) || !handVtable) return nullptr;
    }
    alignas(8) uint8_t raw[off::HandSize] = {};
    std::memcpy(raw, &handVtable, 8);
    std::memcpy(raw + off::H_type, &h.type, 4);
    std::memcpy(raw + off::H_container, &h.container, 4);
    std::memcpy(raw + off::H_containerSerial, &h.containerSerial, 4);
    std::memcpy(raw + off::H_index, &h.index, 4);
    std::memcpy(raw + off::H_serial, &h.serial, 4);
    void* obj = CallResolve(FnAddr(FnHandleResolve), reinterpret_cast<void*>(Addr(rva::HandleTable)), raw);
    if (!IsCharacter(obj)) return nullptr;
    kc::Handle check;   // the resolver must hand back exactly that object
    return GetHandle(static_cast<Character*>(obj), check) && check == h ? static_cast<Character*>(obj) : nullptr;
}

bool GetHandle(const Character* c, kc::Handle& out) {
    return IsCharacter(c) && ReadHandle(reinterpret_cast<const uint8_t*>(c) + off::RO_handle, out);
}

bool GetPosition(Character* c, kc::Vec3& out) {
    if (!IsCharacter(c)) return false;
    void* fn = VSlot(c, slot::RO_getPosition);
    float v[3] = {};
    if (!fn || !CallGetVec3(fn, c, v)) return false;
    out = {v[0], v[1], v[2]};
    return Finite(out);
}

bool GetRotation(const Character* c, kc::Quat& out) {
    float q[4];
    if (!IsCharacter(c) || !Rd(c, off::RO_rot, q)) return false;
    out = {q[0], q[1], q[2], q[3]};
    return std::isfinite(q[0]) && std::isfinite(q[1]) && std::isfinite(q[2]) && std::isfinite(q[3]);
}

bool GetMovement(const Character* c, kc::Vec3& dest, bool& moving, float& speed) {
    void* m = Movement(c);
    float d[3];
    if (!m || !Rd(m, off::CM_destination, d) || !Rd(m, off::CM_currentlyMoving, moving) || !Rd(m, off::CM_currentSpeed, speed)) return false;
    dest = {d[0], d[1], d[2]};
    return Finite(dest) && std::isfinite(speed);
}

bool IsDown(Character* c) {
    void* fn = VSlot(c, slot::RO_isUnconcious);
    bool v = false;
    return IsCharacter(c) && fn && CallBool(fn, c, v) && v;
}

Character* AICharacter(const AI* ai) {
    Character* c = nullptr;
    if (!ai || Vtable(ai) != Addr(rva::VtAI) || !Rd(ai, off::AI_me, c)) return nullptr;
    return IsCharacter(c) ? c : nullptr;
}

Character* MedicalCharacter(const void* medical) {
    Character* c = nullptr;
    if (!medical || !Rd(medical, off::MS_me, c) || !IsCharacter(c)) return nullptr;
    return Medical(c) == medical ? c : nullptr;
}

bool ReadVitals(Character* c, kc::EntityVitals& out) {
    void* m = Medical(c);
    if (!m) return false;
    bool unc = false, dead = false;
    if (!Rd(m, off::MS_blood, out.blood) || !Rd(m, off::MS_koTimer, out.koTimer) || !Rd(m, off::MS_hunger, out.hunger) ||
        !Rd(m, off::MS_unconscious, unc) || !Rd(m, off::MS_dead, dead))
        return false;
    if (!std::isfinite(out.blood) || !std::isfinite(out.koTimer) || !std::isfinite(out.hunger)) return false;
    out.flags = uint8_t((unc ? kc::kVitUnconscious : 0) | (dead ? kc::kVitDead : 0));
    out.parts.clear();
    const auto* lk = reinterpret_cast<const uint8_t*>(m) + off::MS_anatomy;
    uint32_t count = 0;
    void** data = nullptr;
    if (!Rd(lk, off::LK_count, count) || !Rd(lk, off::LK_data, data) || !data || count > kc::kMaxBodyParts) return true;
    for (uint32_t i = 0; i < count; ++i) {
        void* part = nullptr;
        kc::PartVitals pv;
        if (!Rd(data, i * sizeof(void*), part) || !part || !Rd(part, off::HP_flesh, pv.flesh) ||
            !Rd(part, off::HP_stun, pv.stun) || !Rd(part, off::HP_bandage, pv.bandage) ||
            !std::isfinite(pv.flesh) || !std::isfinite(pv.stun) || !std::isfinite(pv.bandage)) {
            out.parts.clear();
            return true;
        }
        out.parts.push_back(pv);
    }
    return true;
}

namespace {
constexpr uintptr_t CH_stats = 0x450;   // CharStats* (Character::getStats)
// CharStats fields of every StatsEnumerated with one of its own (from getStatRef's jump table):
// strength, melee attack, labouring, science, engineering, robotics, weapon smithing, armour smithing,
// medic, thieving, turrets, farming, cooking, stealth, athletics, dexterity, melee defence,
// toughness, assassination, swimming, perception, katanas, sabres, hackers, heavy weapons, blunt,
// martial arts, dodge, polearms, crossbows, friendly fire, lockpicking, bow smithing, mass combat.
constexpr uintptr_t kStatOffsets[kc::kStatCount] = {
    0x80, 0x120, 0xE4, 0xE0, 0xCC, 0xDC, 0xD0, 0xD4, 0x98, 0xAC, 0x114, 0xE8, 0xEC, 0xA4, 0x94, 0x88, 0x124,
    0x90, 0xB8, 0xA8, 0x8C, 0xF8, 0xFC, 0x100, 0x108, 0x104, 0x10C, 0xF0, 0x118, 0x110, 0xF4, 0xB0, 0xD8, 0x9C};
constexpr uintptr_t GW_playerPtr = 0x580, PI_faction = 0x2A0, FA_ownerships = 0x80, OW_money = 0x88;

void* StatsOf(Character* c) {
    void* s = nullptr;
    if (!IsCharacter(c) || !Rd(c, CH_stats, s) || !s) return nullptr;
    Character* me = nullptr;
    return Rd(s, 0x10, me) && me == c ? s : nullptr;   // CharStats::me
}
void* PlayerOwnerships() {
    PlayerInterface* pi = Player();
    void* f = nullptr;
    void* o = nullptr;
    if (!pi || !Rd(pi, PI_faction, f) || !f || !Rd(f, FA_ownerships, o)) return nullptr;
    return o;
}
} // namespace

bool ReadStats(Character* c, std::vector<float>& out) {
    out.clear();
    void* s = StatsOf(c);
    if (!s) return false;
    out.resize(kc::kStatCount);
    for (size_t i = 0; i < kc::kStatCount; ++i)
        if (!Rd(s, kStatOffsets[i], out[i]) || !std::isfinite(out[i]) || out[i] < 0.0f || out[i] > 1000.0f) {
            out.clear();
            return false;
        }
    return true;
}

bool WriteStats(Character* c, const std::vector<float>& stats) {
    void* s = StatsOf(c);
    if (!s || stats.size() != kc::kStatCount) return false;
    for (size_t i = 0; i < kc::kStatCount; ++i) {
        float cur = 0;
        if (Rd(s, kStatOffsets[i], cur) && cur != stats[i]) Wr(s, kStatOffsets[i], stats[i]);
    }
    return true;
}

bool ReadPlayerMoney(int32_t& out) {
    void* o = PlayerOwnerships();
    return o && Rd(o, OW_money, out);
}

bool WritePlayerMoney(int32_t money) {
    void* o = PlayerOwnerships();
    int32_t cur = 0;
    if (!o || !Rd(o, OW_money, cur)) return false;
    if (cur != money) Wr(o, OW_money, money);
    return true;
}

bool ReadStat(Character* c, size_t statIndex, float& out) {
    void* s = StatsOf(c);
    return s && statIndex < kc::kStatCount && Rd(s, kStatOffsets[statIndex], out) && std::isfinite(out);
}

bool WriteStat(Character* c, size_t statIndex, float value) {
    void* s = StatsOf(c);
    if (!s || statIndex >= kc::kStatCount || !std::isfinite(value)) return false;
    Wr(s, kStatOffsets[statIndex], value);
    return true;
}

bool GainExperience(Character* c, size_t statIndex, float amount) {
    void* s = StatsOf(c);
    if (!s || statIndex >= kc::kStatCount) return false;
    using FnIncrease = void (*)(float*, float, float);
    auto* field = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(s) + kStatOffsets[statIndex]);
    reinterpret_cast<FnIncrease>(FnAddr(FnIncreaseStat))(field, amount, 100.0f);
    return true;
}

bool CallSay(Character* c, const std::string& text) {
    void* d = CharacterDialogue(c);
    if (!d) return false;
    alignas(8) uint8_t gs[0x28];
    GameStringView(text, gs);
    using FnSay = void (*)(void*, const void*, void*);
    reinterpret_cast<FnSay>(FnAddr(FnDialogueSay))(d, gs, nullptr);
    return true;
}

namespace {
using FnSendEvent = bool (*)(void*, void*, int);
constexpr int kEvPlayerTalkToMe = 1;   // EventTriggerEnum::EV_PLAYER_TALK_TO_ME
bool StartConvSeh(void* fn, void* d, void* pc, bool& result) {
    __try {
        result = reinterpret_cast<FnSendEvent>(fn)(d, pc, kEvPlayerTalkToMe);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
} // namespace

bool CallStartPlayerConversation(Character* npc, Character* pc) {
    void* d = CharacterDialogue(npc);
    bool result = false;
    return d && IsCharacter(pc) && StartConvSeh(FnAddr(FnDialogueSendEvent), d, pc, result) && result;
}

namespace {
bool SendEventOverrideSeh(void* d, void* pc, int ev, bool& result) {
    __try {
        result = reinterpret_cast<bool (*)(void*, void*, int, bool)>(FnAddr(FnDialogueSendEventOverride))(d, pc, ev, true);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool EndDialogueSeh(void* d) {
    __try {
        reinterpret_cast<void (*)(void*, bool)>(FnAddr(FnDialogueEndDialogue))(d, true);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
} // namespace

bool CallDialogueEvent(Character* npc, Character* pc, int ev) {
    void* d = CharacterDialogue(npc);
    bool result = false;
    return d && IsCharacter(pc) && ev > 0 && SendEventOverrideSeh(d, pc, ev, result) && result;
}

namespace {
bool PickupItemSeh(void* pi, void* item) {
    __try { reinterpret_cast<void (*)(void*, void*)>(FnAddr(FnPickupItem))(pi, item); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
} // namespace

bool OrderPickupItem(Character* c, void* item) {
    PlayerInterface* pi = Player();
    if (!pi || !IsCharacter(c) || !ItemLoose(item)) return false;
    bool ok = false;
    WithSelection(c, [&] { ok = PickupItemSeh(pi, item); });
    return ok;
}

bool FocusCamera(Character* c) {
    PlayerInterface* pi = Player();
    if (!pi || !IsCharacter(c)) return false;
    return WithSelection(c, [&] { reinterpret_cast<void (*)(void*)>(FnAddr(FnFocusCamera))(pi); });
}

namespace {
constexpr uintptr_t CH_platoon = 0x658, CH_squadMemberId = 0x418;   // ActivePlatoon*, int
constexpr uintptr_t AP_platoon = 0x78;                               // ActivePlatoon -> Platoon (named RootObject)
using FnAddAt = void (*)(void*, void*, int);
using FnNewSquad = void* (*)(void*);
using FnSetName = void (*)(void*, const void*);
bool AddAtSeh(void* fn, void* sq, void* c, int index) {
    __try { reinterpret_cast<FnAddAt>(fn)(sq, c, index); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void* NewSquadSeh(void* fn, void* pi) {
    __try { return reinterpret_cast<FnNewSquad>(fn)(pi); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
bool SetNameSeh(void* fn, void* sq, const void* gs) {
    __try { reinterpret_cast<FnSetName>(fn)(sq, gs); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
} // namespace

void* SquadOf(Character* c) {
    void* sq = nullptr;
    return IsCharacter(c) && Rd(c, CH_platoon, sq) ? sq : nullptr;
}

int SquadMemberIndex(Character* c) {
    int i = 0;
    return IsCharacter(c) && Rd(c, CH_squadMemberId, i) ? i : 0;
}

bool SquadName(void* squad, std::string& out) {
    void* p = nullptr;
    return squad && Rd(squad, AP_platoon, p) && p && ReadGameString(reinterpret_cast<uint8_t*>(p) + off::RO_name, out);
}

void SetSquadName(void* squad, const std::string& name) {
    std::string cur;
    if (!squad || (SquadName(squad, cur) && cur == name)) return;
    alignas(8) uint8_t gs[0x28];
    GameStringView(name, gs);
    SetNameSeh(FnAddr(FnSquadSetName), squad, gs);
}

void SquadMembers(void* squad, std::vector<Character*>& out) {
    out.clear();
    std::vector<Character*> all;
    PlayerCharacters(all);
    for (Character* c : all) if (SquadOf(c) == squad) out.push_back(c);
    std::sort(out.begin(), out.end(), [](Character* a, Character* b) { return SquadMemberIndex(a) < SquadMemberIndex(b); });
}

bool MoveToSquad(void* squad, Character* c, int index) {
    // never a dead character, never into or out of the dead squad: the game alone moves the dead
    // there, and a dead character in a regular squad gets a portrait in the squad bar
    if (!squad || !IsCharacter(c) || IsDead(c) || IsDeadSquad(squad) || IsDeadSquad(SquadOf(c))) return false;
    return AddAtSeh(FnAddr(FnSquadAddCharacterAt), squad, c, index);
}

void* ShownSquad() {
    PlayerInterface* pi = Player();
    void* platoon = nullptr;
    if (!pi || !Rd(pi, 0x2A8, platoon) || !platoon) return nullptr;
    void* active = nullptr;
    return Rd(platoon, 0x1D8, active) ? active : nullptr;   // Platoon::activePlatoon
}

void ShowSquad(void* squad) {
    PlayerInterface* pi = Player();
    void* platoon = nullptr;
    if (!pi || !squad || !Rd(squad, AP_platoon, platoon) || !platoon) return;
    using FnShow = bool (*)(void*, void*);
    __try { reinterpret_cast<FnShow>(FnAddr(FnSetCurrentPlatoon))(pi, platoon); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void* NewSquad() {
    PlayerInterface* pi = Player();
    return pi ? NewSquadSeh(FnAddr(FnCreateSquad), pi) : nullptr;
}

namespace { void* FindGameData(const std::string& sid); }   // below

namespace {
// Character -> AnimationClass (+0x448) -> AppearanceBase (+0xE8) -> appearance GameData (+0x148)
constexpr uintptr_t CH_anim = 0x448, ANIM_appearance = 0xE8, APP_data = 0x148;
// GameData value maps (boost unordered_map<std::string, T>): nodes hold the key at +0x10 and the
// value at +0x38; operator[] returns the pair (key, value): value at +0x28.
constexpr uintptr_t GDM_bool = 0xF8, GDM_string = 0x138, GDM_int = 0x178, GDM_float = 0x1B8, GDM_vec = 0x238, GDM_quat = 0x278, GDM_refs = 0x2B8;
constexpr uintptr_t kPairValue = 0x28, kNodeValue = 0x38;
constexpr size_t kRefSize = 0x40;            // GameDataReference: values, sid string (+0x10), GameData* (+0x38)
constexpr uintptr_t kTheGui = 0x21337B0, GUI_editor = 0x1C0;
constexpr uintptr_t ED_charsData = 0x260, ED_charsCount = 0x268;
constexpr uintptr_t kLektorCharVt = 0x16EFE88;
constexpr int kEditDebug = 2;                // CharacterEditMode: race and gender can change, nothing randomised
constexpr int kItemTypeRace = 7;

void* AppearanceData(Character* c) {
    void* anim = nullptr;
    void* app = nullptr;
    void* gd = nullptr;
    if (!IsCharacter(c) || !Rd(c, CH_anim, anim) || !anim || !Rd(anim, ANIM_appearance, app) || !app || !Rd(app, APP_data, gd)) return nullptr;
    return gd;
}

template <typename F>
void ForEachNode(const void* gd, uintptr_t map, F&& f) {
    const auto* m = reinterpret_cast<const uint8_t*>(gd) + map;
    uint64_t size = 0, bucketCount = 0;
    void** buckets = nullptr;
    if (!Rd(m, off::US_size, size) || size == 0 || size > 4096 || !Rd(m, off::US_bucketCount, bucketCount) || !Rd(m, off::US_buckets, buckets) || !buckets)
        return;
    void* node = nullptr;
    if (!Rd(buckets, bucketCount * sizeof(void*), node)) return;
    std::string key;
    for (uint64_t i = 0; node && i < size; ++i) {
        if (ReadGameString(reinterpret_cast<uint8_t*>(node) + off::MapNode_key, key)) f(key, reinterpret_cast<uint8_t*>(node) + kNodeValue);
        if (!Rd(node, off::USNode_next, node)) break;
    }
}

using FnMapIndex = uint8_t* (*)(void* map, const void* key);
uint8_t* MapSlotSeh(void* fn, void* map, const void* key) {
    __try { return reinterpret_cast<FnMapIndex>(fn)(map, key); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
// The value slot for that key (inserted by the game's own operator[] when missing).
uint8_t* ValueSlot(void* gd, uintptr_t map, Fn fn, const std::string& key) {
    alignas(8) uint8_t gs[0x28];
    GameStringView(key, gs);
    uint8_t* pair = MapSlotSeh(FnAddr(fn), reinterpret_cast<uint8_t*>(gd) + map, gs);
    return pair ? pair + kPairValue : nullptr;
}
using FnAssign = void* (*)(void* dst, const void* src, size_t pos, size_t n);
bool AssignSeh(void* dst, const void* src) {
    __try { reinterpret_cast<FnAssign>(FnAddr(FnStringAssign))(dst, src, 0, size_t(-1)); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool AssignGameString(void* dst, const std::string& value) {
    std::string cur;
    if (ReadGameString(dst, cur) && cur == value) return true;
    alignas(8) uint8_t gs[0x28];
    GameStringView(value, gs);
    return AssignSeh(dst, gs);
}
using FnSetAppearance = void (*)(void* c, void* gd);
bool SetAppearanceSeh(void* c, void* gd) {
    __try { reinterpret_cast<FnSetAppearance>(FnAddr(FnSetAppearanceData))(c, gd); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
struct CharLektor {
    uintptr_t vt;
    uint32_t count, capacity;
    void** data;
};
using FnShowEditor = void (*)(void* gui, CharLektor* chars, int mode, const void* races);
bool ShowEditorSeh(CharLektor* lk) {
    __try { reinterpret_cast<FnShowEditor>(FnAddr(FnShowCharacterEditor))(reinterpret_cast<void*>(Addr(kTheGui)), lk, kEditDebug, nullptr); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
using FnGameNew = void* (*)(size_t);
} // namespace

bool GameDataBoolField(const void* gd, const std::string& key, bool& out) {
    bool found = false;
    if (gd) ForEachNode(gd, GDM_bool, [&](const std::string& k, const uint8_t* v) { if (!found && k == key) { out = *v != 0; found = true; } });
    return found;
}

bool GameDataIntField(const void* gd, const std::string& key, int& out) {
    bool found = false;
    if (gd) ForEachNode(gd, GDM_int, [&](const std::string& k, const uint8_t* v) { if (!found && k == key) found = Rd(v, 0, out); });
    return found;
}

bool ReadAppearance(Character* c, kc::AppearanceMsg& out) {
    out = kc::AppearanceMsg{};
    void* gd = AppearanceData(c);
    if (!gd) return false;
    CharacterName(c, out.name);
    using T = kc::AppearanceType;
    ForEachNode(gd, GDM_bool, [&](const std::string& k, const uint8_t* v) { kc::AppearanceField f; f.type = T::Bool; f.key = k; f.b = *v != 0; out.fields.push_back(f); });
    ForEachNode(gd, GDM_int, [&](const std::string& k, const uint8_t* v) { kc::AppearanceField f; f.type = T::Int; f.key = k; Rd(v, 0, f.i); out.fields.push_back(f); });
    ForEachNode(gd, GDM_float, [&](const std::string& k, const uint8_t* v) { kc::AppearanceField f; f.type = T::Float; f.key = k; Rd(v, 0, f.f[0]); out.fields.push_back(f); });
    ForEachNode(gd, GDM_string, [&](const std::string& k, const uint8_t* v) { kc::AppearanceField f; f.type = T::String; f.key = k; ReadGameString(v, f.s); out.fields.push_back(f); });
    ForEachNode(gd, GDM_vec, [&](const std::string& k, const uint8_t* v) { kc::AppearanceField f; f.type = T::Vec3; f.key = k; SafeCopy(f.f, v, 12); out.fields.push_back(f); });
    ForEachNode(gd, GDM_quat, [&](const std::string& k, const uint8_t* v) { kc::AppearanceField f; f.type = T::Quat; f.key = k; SafeCopy(f.f, v, 16); out.fields.push_back(f); });
    ForEachNode(gd, GDM_refs, [&](const std::string& k, const uint8_t* v) {
        kc::AppearanceField f;
        f.type = T::Refs;
        f.key = k;
        const uint8_t* b = nullptr;
        const uint8_t* e = nullptr;
        if (Rd(v, 0, b) && Rd(v, 8, e) && b && e >= b && size_t(e - b) / kRefSize <= 64)
            for (const uint8_t* r = b; r < e; r += kRefSize) { std::string sid; if (ReadGameString(r + 0x10, sid)) f.refs.push_back(sid); }
        out.fields.push_back(f);
    });
    for (auto& f : out.fields) for (float& x : f.f) if (!std::isfinite(x)) x = 0;
    if (out.fields.size() > kc::kMaxAppearanceFields) out.fields.resize(kc::kMaxAppearanceFields);
    return true;
}

bool WriteAppearance(Character* c, const kc::AppearanceMsg& m) {
    void* gd = AppearanceData(c);
    if (!gd) return false;
    using T = kc::AppearanceType;
    int written = 0;
    for (const auto& f : m.fields) {
        uint8_t* v = nullptr;
        switch (f.type) {
        case T::Bool: if ((v = ValueSlot(gd, GDM_bool, FnMapBool, f.key))) { const uint8_t b = f.key == "in editor" ? 0 : f.b; Wr(v, 0, b); } break;
        case T::Int: if ((v = ValueSlot(gd, GDM_int, FnMapInt, f.key))) Wr(v, 0, f.i); break;
        case T::Float: if ((v = ValueSlot(gd, GDM_float, FnMapFloat, f.key))) Wr(v, 0, f.f[0]); break;
        case T::String: if ((v = ValueSlot(gd, GDM_string, FnMapString, f.key))) AssignGameString(v, f.s); break;
        case T::Vec3: if ((v = ValueSlot(gd, GDM_vec, FnMapVec3, f.key))) { for (int k = 0; k < 3; ++k) Wr(v, size_t(k) * 4, f.f[k]); } break;
        case T::Quat: if ((v = ValueSlot(gd, GDM_quat, FnMapQuat, f.key))) { for (int k = 0; k < 4; ++k) Wr(v, size_t(k) * 4, f.f[k]); } break;
        case T::Refs: {
            // only an existing list of the same length is rewritten (the race: one entry)
            ForEachNode(gd, GDM_refs, [&](const std::string& k, const uint8_t* lst) {
                if (k != f.key) return;
                uint8_t* b = nullptr;
                uint8_t* e = nullptr;
                if (!Rd(lst, 0, b) || !Rd(lst, 8, e) || !b || size_t(e - b) / kRefSize != f.refs.size()) return;
                for (size_t i = 0; i < f.refs.size(); ++i) {
                    uint8_t* r = b + i * kRefSize;
                    void* target = FindGameData(f.refs[i]);
                    if (!target) continue;
                    AssignGameString(r + 0x10, f.refs[i]);
                    Wr(r, 0x38, target);
                }
                v = b;
            });
            break;
        }
        }
        written += v != nullptr;
    }
    if (!m.name.empty()) {
        std::string cur;
        if (!CharacterName(c, cur) || cur != m.name)
            if (void* fn = VSlot(c, slot::RO_setName)) { alignas(8) uint8_t gs[0x28]; GameStringView(m.name, gs); CallStrBool(fn, c, gs, false); }
    }
    // same data object: the game sees what changed (race, gender) and rebuilds the body
    return written > 0 && SetAppearanceSeh(c, gd);
}

bool OpenCharacterEditor(Character* c) {
    if (!IsCharacter(c)) return false;
    void** data = static_cast<void**>(reinterpret_cast<FnGameNew>(Addr(0xED6504))(sizeof(void*)));
    if (!data) return false;
    data[0] = c;
    // by value: the callee owns this copy and frees its storage with the game's delete
    CharLektor lk{Addr(kLektorCharVt), 1, 1, data};
    return ShowEditorSeh(&lk);
}

bool CharacterEditorOpen() {
    void* ed = nullptr;
    return Rd(reinterpret_cast<void*>(Addr(kTheGui)), GUI_editor, ed) && ed;
}

void EditorCharacters(std::vector<Character*>& out) {
    out.clear();
    void* ed = nullptr;
    void** data = nullptr;
    uint32_t count = 0;
    if (!Rd(reinterpret_cast<void*>(Addr(kTheGui)), GUI_editor, ed) || !ed || !Rd(ed, ED_charsData, data) || !Rd(ed, ED_charsCount, count) || !data ||
        count > 64)
        return;
    for (uint32_t i = 0; i < count; ++i) {
        void* c = nullptr;
        if (Rd(data, i * sizeof(void*), c) && IsCharacter(c)) out.push_back(static_cast<Character*>(c));
    }
}

namespace {
constexpr uintptr_t CH_stealth = 0xD4;                 // bool, set by the STEALTH_ON / OFF orders
constexpr uintptr_t ST_defensive = 0x128, ST_ranged = 0x129, ST_taunt = 0x12A, ST_hold = 0x12B, ST_passive = 0x12C;   // CharStats
constexpr uintptr_t AI_fightStyle = 0x2B8;             // int: 0 attack, 1 defend, 2 evade
using FnStanding = void (*)(void*, int, bool);
bool StandingSeh(void* c, int order, bool on) {
    __try { reinterpret_cast<FnStanding>(FnAddr(FnSetStandingOrder))(c, order, on); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
} // namespace

namespace {
constexpr uintptr_t CH_isCarrying = 0x348, CH_carrying = 0x380;   // bool, hand
using FnPick = void (*)(void*, void*);
using FnDrop = void (*)(void*, bool, bool);
bool PickSeh(void* c, void* who) {
    __try { reinterpret_cast<FnPick>(FnAddr(FnPickupCharacter))(c, who); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool DropSeh(void* c, bool ragdoll) {
    __try { reinterpret_cast<FnDrop>(FnAddr(FnDropCarried))(c, ragdoll, false); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
} // namespace

bool ReadCarried(Character* c, kc::Handle& carried) {
    uint8_t on = 0;
    return IsCharacter(c) && Rd(c, CH_isCarrying, on) && on && ReadHandle(reinterpret_cast<uint8_t*>(c) + CH_carrying, carried) && carried.valid();
}

// pickupObject (0x5CFF90) silently refuses a body whose ragdoll flag (+0x3d4) is set, and its attach
// step (0x5CED90) sets that flag again on the carried body. A client keeps knocked-out bodies in
// ragdoll (posture sync), so the ragdoll goes first. True only when the body really is on the shoulder.
bool CarryCharacter(Character* carrier, Character* who) {
    if (!IsCharacter(carrier) || !IsCharacter(who)) return false;
    if (IsRagdoll(who)) SetRagdoll(who, false);
    kc::Handle got;
    return PickSeh(carrier, who) && ReadCarried(carrier, got) && Resolve(got) == who;
}

bool DropCarried(Character* carrier, bool ragdoll) {
    return IsCharacter(carrier) && DropSeh(carrier, ragdoll);
}

uint16_t ReadModes(Character* c, uint8_t& style) {
    style = 0;
    if (!IsCharacter(c)) return 0;
    uint16_t m = 0;
    uint8_t b = 0;
    if (Rd(c, CH_stealth, b) && b) m |= kc::kModeStealth;
    if (void* st = StatsOf(c)) {
        if (Rd(st, ST_defensive, b) && b) m |= kc::kModeDefensive;
        if (Rd(st, ST_ranged, b) && b) m |= kc::kModeRanged;
        if (Rd(st, ST_taunt, b) && b) m |= kc::kModeTaunt;
        if (Rd(st, ST_hold, b) && b) m |= kc::kModeHold;
        if (Rd(st, ST_passive, b) && b) m |= kc::kModePassive;
    }
    if (GetStandingOrder(c, 15)) m |= kc::kModeChase;
    void* ai = nullptr;
    int fs = 0;
    if (Rd(c, off::CH_ai, ai) && ai && Rd(ai, AI_fightStyle, fs) && fs >= 0 && fs <= 2) style = uint8_t(fs);
    return m;
}

bool GetStandingOrder(Character* c, int order) {
    if (!IsCharacter(c)) return false;
    uint8_t b = 0;
    void* st = StatsOf(c);
    switch (order) {
    case 3: return Rd(c, CH_stealth, b) && b;   // STEALTH_ON
    case 11: return st && Rd(st, ST_defensive, b) && b;
    case 17: return st && Rd(st, ST_ranged, b) && b;
    case 14: return st && Rd(st, ST_taunt, b) && b;
    case 12: return st && Rd(st, ST_hold, b) && b;
    case 13: return st && Rd(st, ST_passive, b) && b;
    case 15: {   // CHASE: AI -> +0x20 -> +0x35
        void* ai = nullptr;
        void* x = nullptr;
        return Rd(c, off::CH_ai, ai) && ai && Rd(ai, 0x20, x) && x && Rd(x, 0x35, b) && b;
    }
    default: return false;
    }
}

void SetStandingOrder(Character* c, int order, bool on) {
    if (IsCharacter(c)) StandingSeh(c, order, on);
}

void ObjectsNear(const kc::Vec3& pos, float radius, std::vector<void*>& out) {
    out.clear();
    std::vector<void*> all;
    AllObjectsNear(pos, radius, all);
    for (void* o : all) if (!IsCharacter(o)) out.push_back(o);
}

bool ObjectPosition(void* obj, kc::Vec3& out) {
    void* fn = obj ? VSlot(obj, slot::RO_getPosition) : nullptr;
    float v[3] = {};
    if (!fn || !CallGetVec3(fn, obj, v)) return false;
    out = {v[0], v[1], v[2]};
    return Finite(out);
}

bool TemplateDisplayName(const std::string& sid, std::string& out) {
    void* gd = FindGameData(sid);
    return gd && ReadGameString(reinterpret_cast<uint8_t*>(gd) + 0x28, out) && !out.empty();   // GameData::name
}

bool ObjectTemplate(const void* obj, std::string& sid) {
    void* gd = nullptr;
    return obj && Rd(obj, off::RO_data, gd) && gd && ReadGameString(reinterpret_cast<uint8_t*>(gd) + off::GD_stringID, sid) && !sid.empty();
}

bool IsStatOfCharacter(const void* statField) {
    // the stat functions get `this` + offset: walk back to a CharStats whose `me` points back at it
    const auto p = reinterpret_cast<uintptr_t>(statField);
    for (uintptr_t off : kStatOffsets) {
        Character* me = nullptr;
        void* back = nullptr;
        const auto s = reinterpret_cast<void*>(p - off);
        if (Rd(s, 0x10, me) && IsCharacter(me) && Rd(me, CH_stats, back) && back == s) return true;
    }
    return false;
}

namespace {
// A platoon the player still has (the squad of one of the player's characters). The squad bar's
// platoon saved before a call that moves characters (MoveToSquad, an order) may be deleted with its
// last member by then: showing it again would build the squad bar from freed memory.
bool PlayerHasPlatoon(void* platoon) {
    if (!platoon) return false;
    std::vector<Character*> all;
    PlayerCharacters(all);
    for (Character* c : all) {
        void* sq = SquadOf(c);
        void* p = nullptr;
        if (sq && Rd(sq, AP_platoon, p) && p == platoon) return true;
    }
    return false;
}
int g_selectionRestoreFailures = 0;   // the player's selection could not be put back exactly
void ShowPlatoonSeh(void* pi, void* platoon) {
    using FnShow = bool (*)(void*, void*);
    __try { reinterpret_cast<FnShow>(FnAddr(FnSetCurrentPlatoon))(pi, platoon); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
} // namespace

// ---- the selection the game's "order the selected characters" functions act on
// Evidence (1.0.68 disassembly): PlayerInterface::unselectAll (0x7F8DA0) does NOT leave the
// selection empty: it reselects the "main" selected character (the global hand 0x21345D0, or the
// first one of the selection when the details panel's hand is not in it), and
// objectSelected(obj, false) (0x7F7F20) refuses to take out the last selected character. The game
// never empties the selection itself. "unselectAll + select the client's character" therefore left
// the host's main character selected too, and the order went to both (addOrder / newPlayerTask /
// addJob) or to the nearest of them (addTaskNearest: a bed, a conversation): the host's character
// went to sleep or talked instead of the client's. Only an empty host selection (right after
// loading, as in the test harness) was safe.
namespace {
constexpr uintptr_t PI_detailsHand = 0xF0, PI_currentPlatoon = 0x2A8;
constexpr uintptr_t kRvaMainSelectedHand = 0x21345D0;   // hand objectSelected writes, unselectAll resets
using FnSelSig = void (*)(void*, void*, bool);
using FnClearSig = void (*)(void*);
bool SelectSeh(void* pi, void* o, bool on) {
    __try { reinterpret_cast<FnSelSig>(FnAddr(FnObjectSelected))(pi, o, on); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool UnselectAllSeh(void* pi) {
    __try { reinterpret_cast<FnClearSig>(FnAddr(FnUnselectAll))(pi); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void* SelectableObject(const kc::Handle& h) {
    void* o = Resolve(h);
    return o ? o : ResolveItem(h);
}
bool Contains(const std::vector<kc::Handle>& v, const kc::Handle& h) { return std::find(v.begin(), v.end(), h) != v.end(); }
bool SameSelection(const std::vector<kc::Handle>& a, const std::vector<kc::Handle>& b) {
    if (a.size() != b.size()) return false;
    for (const auto& h : a) if (!Contains(b, h)) return false;
    return true;
}
uint8_t* MainSelectedHand() { return reinterpret_cast<uint8_t*>(Addr(kRvaMainSelectedHand)); }

struct SavedSelection {
    std::vector<kc::Handle> sel;
    uint8_t details[off::HandSize] = {};
    uint8_t mainHand[off::HandSize] = {};
    void* platoon = nullptr;
};
void SaveSelection(PlayerInterface* pi, SavedSelection& s) {
    SelectedHandles(s.sel);
    SafeCopy(s.details, reinterpret_cast<uint8_t*>(pi) + PI_detailsHand, sizeof(s.details));
    SafeCopy(s.mainHand, MainSelectedHand(), sizeof(s.mainHand));
    Rd(pi, PI_currentPlatoon, s.platoon);
}
void WriteBytes(uint8_t* dst, const uint8_t* src, size_t n) {
    for (size_t k = 0; k < n; ++k) Wr(dst, k, src[k]);
}
// Takes every character out of the selection, the last one included: unselectAll reselects the
// object of the main hand when the details hand is in the selection, so the main hand is made null
// (what unselectAll itself writes: type 0xB, the rest 0) and the details hand names a selected one.
bool EmptySelection(PlayerInterface* pi) {
    UnselectAllSeh(pi);
    std::vector<kc::Handle> now;
    SelectedHandles(now);
    if (now.empty()) return true;
    alignas(8) uint8_t hand[off::HandSize];
    MakeHand(now.front(), hand);
    WriteBytes(reinterpret_cast<uint8_t*>(pi) + PI_detailsHand + 8, hand + 8, off::HandSize - 8);
    alignas(8) uint8_t nullHand[off::HandSize] = {};
    const uint32_t noType = 0xB;
    std::memcpy(nullHand + off::H_type, &noType, 4);
    WriteBytes(MainSelectedHand() + 8, nullHand + 8, off::HandSize - 8);
    UnselectAllSeh(pi);
    SelectedHandles(now);
    return now.empty();
}
// Puts the selection, the squad bar, the details panel and the main hand back as they were.
bool RestoreSelection(PlayerInterface* pi, const SavedSelection& s) {
    UnselectAllSeh(pi);   // keeps at most one: the main one, maybe a character the call selected
    for (const auto& h : s.sel)
        if (void* o = SelectableObject(h)) SelectSeh(pi, o, true);
    std::vector<kc::Handle> now;
    SelectedHandles(now);
    for (const auto& h : now)
        if (!Contains(s.sel, h))
            if (void* o = SelectableObject(h)) SelectSeh(pi, o, false);   // refused for the last one: see below
    if (s.sel.empty()) EmptySelection(pi);
    void* nowPlatoon = nullptr;
    // only a squad the player still has: the call may have deleted it with its last member, and
    // showing it would build the squad bar from freed memory
    if (s.platoon && Rd(pi, PI_currentPlatoon, nowPlatoon) && nowPlatoon != s.platoon && PlayerHasPlatoon(s.platoon))
        ShowPlatoonSeh(pi, s.platoon);
    WriteBytes(reinterpret_cast<uint8_t*>(pi) + PI_detailsHand, s.details, sizeof(s.details));
    WriteBytes(MainSelectedHand(), s.mainHand, sizeof(s.mainHand));
    SelectedHandles(now);
    return SameSelection(now, s.sel);
}
} // namespace

bool SelectExactly(const std::vector<Character*>& actors, std::string* why) {
    PlayerInterface* pi = Player();
    if (!pi || actors.empty()) { if (why) *why = !pi ? "no player interface" : "no actor"; return false; }
    std::vector<kc::Handle> want;
    for (Character* c : actors) {
        kc::Handle h;
        if (!IsCharacter(c) || !GetHandle(c, h)) { if (why) *why = "an actor is not a character"; return false; }
        if (!Contains(want, h)) want.push_back(h);
    }
    UnselectAllSeh(pi);   // leaves the main one selected (see above)
    for (Character* c : actors) SelectSeh(pi, c, true);
    std::vector<kc::Handle> now;
    SelectedHandles(now);
    for (const auto& h : now)
        if (!Contains(want, h))
            if (void* o = SelectableObject(h)) SelectSeh(pi, o, false);   // possible once an actor is selected
    SelectedHandles(now);
    if (SameSelection(now, want)) return true;
    if (why) {
        size_t actorsIn = 0;
        for (const auto& h : want) actorsIn += Contains(now, h) ? 1 : 0;
        char b[160];
        snprintf(b, sizeof(b), "the selection is %zu character(s), %zu of the %zu actor(s): an actor cannot be selected here", now.size(), actorsIn,
                 want.size());
        *why = b;
    }
    return false;
}

bool WithSelection(const std::vector<Character*>& actors, const std::function<void()>& fn, std::string* why) {
    PlayerInterface* pi = Player();
    if (!pi || actors.empty()) { if (why) *why = !pi ? "no player interface" : "no actor"; return false; }
    // everything selecting a character changes: the selection, the squad the squad bar shows, the
    // character whose details panel is open, the main hand; all of it comes back exactly as it was
    SavedSelection saved;
    SaveSelection(pi, saved);
    const bool exact = SelectExactly(actors, why);
    if (exact) fn();   // never with anything else selected: the order would go to it too
    const bool restored = RestoreSelection(pi, saved);
    if (!restored) {
        ++g_selectionRestoreFailures;
        if (why && exact) *why = "the order ran, but the player's selection could not be put back exactly";
    }
    return exact;
}

bool WithSelection(Character* only, const std::function<void()>& fn) {
    return WithSelection(std::vector<Character*>{only}, fn, nullptr);
}

int SelectionRestoreFailures() { return g_selectionRestoreFailures; }

namespace {
constexpr uintptr_t CH_dialogue = 0x280;    // Dialogue*
constexpr uintptr_t DL_hasEnded = 0x148, DL_shouting = 0x149, DL_me = 0x150, DL_target = 0x158;   // bool, bool, Character*, hand
constexpr uintptr_t DL_responses = 0x258, DL_npcReply = 0x278;               // vector<std::string>, std::string
constexpr size_t kGameStringSize = 0x28;
} // namespace

void* CharacterDialogue(Character* c) {
    void* d = nullptr;
    if (!IsCharacter(c) || !Rd(c, CH_dialogue, d) || !d) return nullptr;
    Character* me = nullptr;
    return Rd(d, DL_me, me) && me == c ? d : nullptr;
}

Character* DialogueOwner(const void* dialogue) {
    Character* me = nullptr;
    if (!dialogue || !Rd(dialogue, DL_me, me) || !IsCharacter(me)) return nullptr;
    void* back = nullptr;
    return Rd(me, CH_dialogue, back) && back == dialogue ? me : nullptr;
}

Character* DialogueTarget(const void* dialogue) {
    kc::Handle h;
    if (!dialogue || !ReadHandle(reinterpret_cast<const uint8_t*>(dialogue) + DL_target, h) || !h.valid()) return nullptr;
    return Resolve(h);
}

bool DialogueShouting(const void* dialogue) {
    bool v = false;
    return dialogue && Rd(dialogue, DL_shouting, v) && v;
}

void SetDialogueShouting(void* dialogue, bool shout) {
    if (dialogue) Wr(dialogue, DL_shouting, shout);
}

bool DialogueEnded(const void* dialogue) {
    bool v = false;
    return dialogue && Rd(dialogue, DL_hasEnded, v) && v;
}

bool CallEndDialogue(void* dialogue) {
    return DialogueOwner(dialogue) && EndDialogueSeh(dialogue);
}

bool ReadDialogueWindowText(const void* dialogue, std::string& text, std::vector<std::string>& replies) {
    text.clear();
    replies.clear();
    if (!dialogue || !ReadGameString(reinterpret_cast<const uint8_t*>(dialogue) + DL_npcReply, text)) return false;
    const uint8_t* first = nullptr;
    const uint8_t* last = nullptr;
    const auto* v = reinterpret_cast<const uint8_t*>(dialogue) + DL_responses;
    if (!Rd(v, 0, first) || !Rd(v, 8, last) || !first || last < first) return true;
    const size_t n = size_t(last - first) / kGameStringSize;
    for (size_t i = 0; i < n && i < kc::kMaxDialogReplies; ++i) {
        std::string s;
        if (!ReadGameString(first + i * kGameStringSize, s)) break;
        replies.push_back(std::move(s));
    }
    return true;
}

void GameStringView(const std::string& s, void* out) {
    auto* raw = static_cast<uint8_t*>(out);
    std::memset(raw, 0, kGameStringSize);
    const uint64_t size = s.size();
    if (s.size() < 16) {
        std::memcpy(raw, s.data(), s.size());
        const uint64_t cap = 15;
        std::memcpy(raw + 0x18, &cap, 8);
    } else {
        const char* p = s.c_str();
        std::memcpy(raw, &p, 8);
        std::memcpy(raw + 0x18, &size, 8);
    }
    std::memcpy(raw + 0x10, &size, 8);
}

bool WriteVitals(Character* c, const kc::EntityVitals& v) {
    void* m = Medical(c);
    if (!m) return false;
    Wr(m, off::MS_blood, v.blood);
    Wr(m, off::MS_koTimer, v.koTimer);
    Wr(m, off::MS_hunger, v.hunger);
    // The local medical update may decide on its own that the character died or fainted (it sets
    // these before asking for the death, which clients refuse): the host's state wins. Falling and
    // dying themselves are replayed separately, so only the "it did not happen" side is written.
    const bool no = false;
    bool flag = false;
    if (!(v.flags & kc::kVitDead) && Rd(m, off::MS_dead, flag) && flag) Wr(m, off::MS_dead, no);
    if (!(v.flags & (kc::kVitDead | kc::kVitUnconscious)) && v.koTimer <= 0 && Rd(m, off::MS_unconscious, flag) && flag)
        Wr(m, off::MS_unconscious, no);
    const auto* lk = reinterpret_cast<const uint8_t*>(m) + off::MS_anatomy;
    uint32_t count = 0;
    void** data = nullptr;
    // same save + same race => same anatomy; anything else is left alone
    if (!Rd(lk, off::LK_count, count) || !Rd(lk, off::LK_data, data) || !data || count != v.parts.size()) return true;
    for (uint32_t i = 0; i < count; ++i) {
        void* part = nullptr;
        if (!Rd(data, i * sizeof(void*), part) || !part) continue;
        Wr(part, off::HP_flesh, v.parts[i].flesh);
        Wr(part, off::HP_stun, v.parts[i].stun);
        Wr(part, off::HP_bandage, v.parts[i].bandage);
    }
    return true;
}

bool IsDead(Character* c) {
    bool v = false;
    void* m = Medical(c);
    return m && Rd(m, off::MS_dead, v) && v;
}

bool IsUnconscious(Character* c) {
    bool v = false;
    void* m = Medical(c);
    return m && Rd(m, off::MS_unconscious, v) && v;
}

void SetUnconscious(Character* c, bool on) {
    if (void* m = Medical(c)) Wr(m, off::MS_unconscious, on);
}

bool IsRagdoll(Character* c) {
    bool v = false;
    return IsCharacter(c) && CallBool(FnAddr(FnIsRagdoll), c, v) && v;
}

bool CallDeclareDead(Character* c) {
    return IsCharacter(c) && CallVoid(FnAddr(FnDeclareDead), c);
}

bool CallKnockout(Character* c) {
    void* m = Medical(c);
    return m && CallMedFloat(FnAddr(FnMedKnockout), m, 0.0f);
}

bool ForceKnockout(Character* c, float seconds) {
    // knockout() only arms the timer (+0xA0): nothing makes the character unconscious until damage
    // or stun does, so a standing NPC kept standing with ko=3. The state and the fall themselves,
    // with a timer long enough for the medical update not to wake it at once.
    void* m = Medical(c);
    if (!m || !CallMedFloat(FnAddr(FnMedKnockout), m, 0.0f)) return false;
    float t = 0;
    if (!Rd(m, off::MS_koTimer, t) || !std::isfinite(t) || t < seconds) Wr(m, off::MS_koTimer, seconds);
    SetUnconscious(c, true);
    SetRagdoll(c, true);
    return IsUnconscious(c);
}

bool GetGameHours(double& out) {
    void* clock = nullptr;
    if (!Rd(reinterpret_cast<void*>(Addr(rva::GameClockOwner)), 0, clock) || !clock) return false;
    return Rd(clock, off::Clock_hours, out) && std::isfinite(out) && out >= 0;
}

// The hours (+0xA0) are recomputed every frame from the day (+0x08) and the sky clock's hour of day,
// so those two are what is set: the day as a new game sets it, the hour through the game's setter
// (it updates the sky and the hour/minute fields, then the hours themselves).
bool SetGameHours(double hours) {
    void* clock = nullptr;
    void* sky = nullptr;
    if (!std::isfinite(hours) || hours < 0 || hours > 24.0 * 1e6) return false;
    if (!Rd(reinterpret_cast<void*>(Addr(rva::GameClockOwner)), 0, clock) || !clock || !Rd(clock, off::Clock_sky, sky) || !sky) return false;
    int32_t day = int32_t(std::floor(hours / 24.0));
    float hourOfDay = float(hours - 24.0 * day);
    if (hourOfDay >= 24.0f) { ++day; hourOfDay = 0.0f; }   // rounding to float
    if (hourOfDay < 0.0f) hourOfDay = 0.0f;
    if (!Wr(clock, off::Clock_day, day)) return false;
    if (!CallMedFloat(FnAddr(FnClockSetHourOfDay), clock, hourOfDay)) return false;
    double now = 0;
    return GetGameHours(now) && std::fabs(now - hours) < 0.01;
}


namespace {
void* SaveManagerInstance() { return CallNoArgPtr(FnAddr(FnSaveManagerGet)); }
}

size_t LocalTaskCount(Character* c) {
    // Character -> AI (+0x650) -> AITaskSytem (+0x20) -> its actions (ActionDeque at +0x300, size at +0x28)
    void* ai = nullptr;
    void* ts = nullptr;
    uint64_t n = 0;
    if (!IsCharacter(c) || !Rd(c, off::CH_ai, ai) || !ai || !Rd(ai, 0x20, ts) || !ts || !Rd(ts, 0x300 + 0x28, n) || n > 1000) return 0;
    return size_t(n);
}

bool DropLocalTasks(Character* c) {
    return IsCharacter(c) && CallVoid(FnAddr(FnReThinkAIAction), c);
}

bool ShowLoadWindow() {
    void* sm = SaveManagerInstance();
    return sm && CallVoid(FnAddr(FnShowLoadWindow), sm);
}

bool SaveManagerBusy() {
    void* sm = SaveManagerInstance();
    int signal = 0;
    return !sm || !Rd(sm, off::SM_signal, signal) || signal != 0;
}

bool SaveFolder(std::string& out) {
    void* sm = SaveManagerInstance();
    bool user = false;
    if (!sm || !Rd(reinterpret_cast<void*>(Addr(rva::SaveUsesUserPath)), 0, user)) return false;
    return ReadGameString(reinterpret_cast<uint8_t*>(sm) + (user ? off::SM_userSavePath : off::SM_localSavePath), out) && !out.empty();
}

bool RequestSave(const std::string& name, std::string* folderOut) {
    void* sm = SaveManagerInstance();
    GameString gs;
    if (!sm || !MakeGameString(name, gs) || SaveManagerBusy()) return false;
    if (!CallStrBool(FnAddr(FnSaveManagerSave), sm, gs.raw, false)) return false;
    // save() silently refuses in some states (editor, another operation): it then leaves no request
    if (!SaveManagerBusy()) return false;
    return !folderOut || ReadGameString(reinterpret_cast<uint8_t*>(sm) + off::SM_location, *folderOut);
}

bool RequestLoad(const std::string& name) {
    void* sm = SaveManagerInstance();
    GameString gs;
    if (!sm || !MakeGameString(name, gs) || SaveManagerBusy()) return false;
    return CallStr(FnAddr(FnSaveManagerLoad), sm, gs.raw);
}

namespace {
std::unordered_map<std::string, void*> g_gameDataBySid;   // built once per world
bool g_gameDataBuilt = false;

using FnCreateChar = void* (*)(void* factory, void* faction, const float* pos, void* owner, void* data, void* home, float age);
void* CallCreateChar(void* fn, void* factory, void* faction, const float* pos, void* data, float age, unsigned long* fault) {
    __try {
        return reinterpret_cast<FnCreateChar>(fn)(factory, faction, pos, nullptr, data, nullptr, age);
    } __except (*fault = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}
using FnDestroy = bool (*)(void* world, void* obj, bool justUnloaded, const char* info);
bool CallDestroy(void* fn, void* world, void* obj) {
    __try {
        return reinterpret_cast<FnDestroy>(fn)(world, obj, false, "KenshiCoop");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
using VirtFloatRet = float (*)(void* self);
bool CallFloatRet(void* fn, void* self, float& out) {
    __try {
        out = reinterpret_cast<VirtFloatRet>(fn)(self);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool GameDataSid(const void* gd, std::string& out) {
    return gd && ReadGameString(reinterpret_cast<const uint8_t*>(gd) + off::GD_stringID, out) && !out.empty();
}

void BuildGameDataIndex() {
    g_gameDataBuilt = true;
    g_gameDataBySid.clear();
    GameWorld* w = World();
    if (!w) return;
    const auto* map = reinterpret_cast<const uint8_t*>(w) + off::GW_gamedataBySid;
    uint64_t size = 0, bucketCount = 0;
    void** buckets = nullptr;
    if (!Rd(map, off::US_size, size) || size == 0 || size > 2000000) return;
    if (!Rd(map, off::US_bucketCount, bucketCount) || !Rd(map, off::US_buckets, buckets) || !buckets) return;
    void* node = nullptr;
    if (!Rd(buckets, bucketCount * sizeof(void*), node)) return;
    size_t bad = 0;
    std::string key, sid;
    for (uint64_t i = 0; node && i < size; ++i) {
        void* gd = nullptr;
        if (ReadGameString(reinterpret_cast<uint8_t*>(node) + off::MapNode_key, key) && Rd(node, off::MapNode_mapped, gd) && gd) {
            // the GameData must agree with its key: a cheap layout self-check
            if (GameDataSid(gd, sid) && sid == key) g_gameDataBySid.emplace(key, gd);
            else ++bad;
        }
        if (!Rd(node, off::USNode_next, node)) break;
    }
    if (bad > g_gameDataBySid.size() / 10) g_gameDataBySid.clear();   // layout not as expected: refuse to use it
}

void* FindGameData(const std::string& sid) {
    if (!g_gameDataBuilt) BuildGameDataIndex();
    auto it = g_gameDataBySid.find(sid);
    return it == g_gameDataBySid.end() ? nullptr : it->second;
}

void* FindFaction(const std::string& sid) {
    GameWorld* w = World();
    void* mgr = nullptr;
    if (!w || !Rd(w, off::GW_factionMgr, mgr) || !mgr) return nullptr;
    uint32_t count = 0;
    void** data = nullptr;
    if (!Rd(mgr, off::LK_count, count) || !Rd(mgr, off::LK_data, data) || !data || count > 10000) return nullptr;
    std::string s;
    for (uint32_t i = 0; i < count; ++i) {
        void* f = nullptr;
        void* gd = nullptr;
        if (Rd(data, i * sizeof(void*), f) && f && Rd(f, off::FAC_data, gd) && GameDataSid(gd, s) && s == sid) return f;
    }
    return nullptr;
}
} // namespace

size_t GameDataIndexSize() { return g_gameDataBySid.size(); }

void ResetLookupCaches() {
    g_gameDataBuilt = false;
    g_gameDataBySid.clear();
}

bool ReadSpawnSource(Character* c, kc::SpawnInfo& out) {
    if (!IsCharacter(c)) return false;
    void* gd = nullptr;
    void* faction = nullptr;
    void* fgd = nullptr;
    if (!Rd(c, off::RO_data, gd) || !GameDataSid(gd, out.templateSid)) return false;
    if (!Rd(c, off::RO_owner, faction) || !faction || !Rd(faction, off::FAC_data, fgd) || !GameDataSid(fgd, out.factionSid)) return false;
    if (!ReadGameString(reinterpret_cast<uint8_t*>(c) + off::RO_name, out.name)) out.name.clear();
    out.age = 0;
    if (void* fn = VSlot(c, slot::CH_getAge)) CallFloatRet(fn, c, out.age);
    if (!std::isfinite(out.age)) out.age = 0;
    return out.templateSid.size() <= kc::kMaxSidLen && out.factionSid.size() <= kc::kMaxSidLen;
}

Character* CreateCharacter(const kc::SpawnInfo& info, const kc::Vec3& pos, std::string* err) {
    GameWorld* w = World();
    void* factory = nullptr;
    if (!w || !Rd(w, off::GW_factory, factory) || !factory) { if (err) *err = "no factory"; return nullptr; }
    void* gd = FindGameData(info.templateSid);
    if (!gd) { if (err) *err = "unknown template " + info.templateSid; return nullptr; }
    void* faction = FindFaction(info.factionSid);
    if (!faction) { if (err) *err = "unknown faction " + info.factionSid; return nullptr; }
    const float p[3] = {pos.x, pos.y, pos.z};
    unsigned long fault = 0;
    void* obj = CallCreateChar(FnAddr(FnCreateRandomCharacter), factory, faction, p, gd, info.age, &fault);
    if (fault && err) {
        // the factory faulted part way: a character it had begun may stay in the game's update lists
        // half set up (Character::update on one whose CharStats has no MedicalSystem yet crashes at
        // kenshi_x64+0x883B78); the caller stops recreating this template
        char buf[160];
        snprintf(buf, sizeof buf, "exception %08lx inside the game's character factory", fault);
        *err = buf;
        return nullptr;
    }
    if (!IsCharacter(obj)) {
        if (err) {
            int type = -1;
            Rd(gd, off::GD_type, type);
            std::string name;
            ReadGameString(reinterpret_cast<const uint8_t*>(gd) + off::GD_name, name);
            char buf[160];
            snprintf(buf, sizeof buf, "factory did not return a character (template type %d '%.60s', got %p vt+%llx)", type, name.c_str(), obj,
                     obj ? static_cast<unsigned long long>(Vtable(obj) - Addr(0)) : 0ull);
            *err = buf;
        }
        return nullptr;
    }
    // Same name as on the host when it fits an inline string (the game never frees it).
    GameString gs;
    if (!info.name.empty() && MakeGameString(info.name, gs))
        if (void* fn = VSlot(obj, slot::RO_setName)) CallStr(fn, obj, gs.raw);
    return static_cast<Character*>(obj);
}

namespace {
using FnRecruitSig = bool (*)(void* pi, void* c, bool editor);
bool CallRecruit(void* fn, void* pi, void* c) {
    __try {
        reinterpret_cast<FnRecruitSig>(fn)(pi, c, false);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
} // namespace

std::string ClassRvas(const Character* c) {
    void* m = nullptr;
    Rd(c, off::CH_movement, m);
    char buf[64];
    snprintf(buf, sizeof buf, "%llx/%llx", static_cast<unsigned long long>(Vtable(c) - g_base),
             static_cast<unsigned long long>(m ? Vtable(m) - g_base : 0));
    return buf;
}

namespace {
using FnAddTaskNearestSig = void (*)(void* pi, void* building, int task, void* subject, bool shift, const float* loc, bool noAnimals);
bool CallAddTaskNearestRaw(void* fn, void* pi, int task, void* subject, const float* loc) {
    __try {
        reinterpret_cast<FnAddTaskNearestSig>(fn)(pi, nullptr, task, subject, false, loc, false);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
} // namespace

bool CallAddTaskNearestObject(int task, void* subject, const kc::Vec3& at) {
    PlayerInterface* pi = Player();
    if (!pi || !subject) return false;
    const float loc[3] = {at.x, at.y, at.z};
    return CallAddTaskNearestRaw(FnAddr(FnAddTaskNearest), pi, task, subject, loc);
}

void* FurnitureParent(void* furniture) {
    if (!furniture || IsCharacter(furniture)) return nullptr;
    std::string sid;
    // Building::isFurnitureOf at +0x238: a pointer, or a hand to resolve
    void* parent = nullptr;
    if (Rd(furniture, 0x238, parent) && parent && parent != furniture && ObjectTemplate(parent, sid)) return parent;
    kc::Handle h;
    if (ReadHandle(reinterpret_cast<uint8_t*>(furniture) + 0x238, h) && h.valid()) {
        void* p = ResolveItem(h);
        if (p && p != furniture && ObjectTemplate(p, sid)) return p;
    }
    return nullptr;
}

namespace {
using FnNewTaskSig = void (*)(void* pi, int task, const void* targetHand, void* building, const float* clickPos, bool addDontClear);
bool NewTaskSeh(void* fn, void* pi, int task, const void* hand, void* building, const float* loc) {
    __try { reinterpret_cast<FnNewTaskSig>(fn)(pi, task, hand, building, loc, false); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
} // namespace

bool CallNewPlayerTaskOn(int task, void* subject, const kc::Vec3& at, void* building) {
    PlayerInterface* pi = Player();
    kc::Handle h;
    if (!pi || !ObjectHandle(subject, h)) return false;
    alignas(8) uint8_t hand[off::HandSize];
    MakeHand(h, hand);
    const float loc[3] = {at.x, at.y, at.z};
    return NewTaskSeh(FnAddr(FnNewPlayerTaskSelected), pi, task, hand, building, loc);
}

bool ReadInSomething(Character* c, int& v) {
    return IsCharacter(c) && Rd(c, 0x2F8, v);
}

bool CallAddTaskNearest(int task, Character* subject) {
    kc::Vec3 p;
    PlayerInterface* pi = Player();
    if (!pi || !GetPosition(subject, p)) return false;
    const float loc[3] = {p.x, p.y, p.z};
    return CallAddTaskNearestRaw(FnAddr(FnAddTaskNearest), pi, task, subject, loc);
}

bool CharacterName(const Character* c, std::string& out) {
    return IsCharacter(c) && ReadGameString(reinterpret_cast<const uint8_t*>(c) + off::RO_name, out);
}

Character* CreateRecruit(Character* model, const std::string& name, const kc::Vec3& pos, std::string* err) {
    kc::SpawnInfo info;
    if (!ReadSpawnSource(model, info)) { if (err) *err = "cannot read the squad's character template"; return nullptr; }
    info.name = name.substr(0, 15);
    Character* c = CreateCharacter(info, pos, err);
    if (!c) return nullptr;
    PlayerInterface* pi = Player();
    if (!pi || !CallRecruit(FnAddr(FnRecruit), pi, c)) { if (err) *err = "recruit() failed"; return nullptr; }
    return c;
}

namespace {
void* CombatOf(const Character* c) {
    void* body = nullptr;
    void* combat = nullptr;
    if (!IsCharacter(c) || !Rd(c, off::CH_body, body) || !body || !Rd(body, off::BODY_combat, combat) || !combat) return nullptr;
    const uintptr_t vt = Vtable(combat);
    return (vt == Addr(rva::VtCombatClass) || vt == Addr(rva::VtCombatClassAI)) ? combat : nullptr;
}
using FnInitCombat = bool (*)(void* self, const void* hand, int end, bool focused);
bool CallInitCombat(void* fn, void* self, const void* hand, int end) {
    __try {
        reinterpret_cast<FnInitCombat>(fn)(self, hand, end, true);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
} // namespace

bool ReadCombat(Character* c, kc::Handle& target) {
    void* combat = CombatOf(c);
    bool active = false;
    if (!combat || !Rd(combat, off::CC_active, active) || !active) return false;
    return ReadHandle(reinterpret_cast<uint8_t*>(combat) + off::CC_target, target) && target.valid();
}

bool StartCombat(Character* c, const kc::Handle& target) {
    void* combat = CombatOf(c);
    if (!combat || !target.valid()) return false;
    alignas(8) uint8_t raw[off::HandSize] = {};
    const uintptr_t vt = Addr(rva::VtHand);
    std::memcpy(raw, &vt, 8);
    std::memcpy(raw + off::H_type, &target.type, 4);
    std::memcpy(raw + off::H_container, &target.container, 4);
    std::memcpy(raw + off::H_containerSerial, &target.containerSerial, 4);
    std::memcpy(raw + off::H_index, &target.index, 4);
    std::memcpy(raw + off::H_serial, &target.serial, 4);
    void* fn = VSlot(combat, slot::CC_initCombatMode);
    return fn && CallInitCombat(fn, combat, raw, 0);
}

bool EndCombat(Character* c) {
    return CombatOf(c) && CallVoid(FnAddr(FnEndCombatMode), c);
}

namespace {
using FnBoolInt = void (*)(void* self, bool b, int i);
bool CallBoolInt(void* fn, void* self, bool b, int i) {
    __try {
        reinterpret_cast<FnBoolInt>(fn)(self, b, i);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
using FnInt = void (*)(void* self, int i);
bool CallInt(void* fn, void* self, int i) {
    __try {
        reinterpret_cast<FnInt>(fn)(self, i);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
constexpr int kRagdollWhole = 1;
constexpr int kProneNormal = 0;
} // namespace

namespace {
// WeatherRegion
constexpr uintptr_t WR_biomeGroup = 0x0, WR_seasonsBegin = 0x8, WR_seasonsEnd = 0x10, WR_instance = 0x30, WR_season = 0x38,
                    WR_seasonIndex = 0x40, WR_seasonEnd = 0x44, WR_effectsDirty = 0x69, WR_newWeather = 0xB1;
constexpr uintptr_t ABG_data = 0x10;                                   // AreaBiomeGroup: GameData*
constexpr uintptr_t SEASON_data = 0x40, SEASON_weatherCount = 0x10, SEASON_weathers = 0x18;
constexpr uintptr_t WEATHER_data = 0x8;
// WeatherInstance
constexpr uintptr_t WI_weather = 0x8, WI_effectStrength = 0x10, WI_strength = 0x14, WI_windSpeed = 0x18, WI_windDir = 0x1C,
                    WI_buEnded = 0x28, WI_buStart = 0x2C, WI_buEnd = 0x30, WI_buSpeedStart = 0x34, WI_buSpeedEnd = 0x38,
                    WI_buAngleStart = 0x3C, WI_buAngleEnd = 0x40, WI_start = 0x44, WI_end = 0x48, WI_updWind = 0x4C, WI_time = 0x50;

bool SidAt(const void* obj, uintptr_t gdOffset, std::string& out) {
    void* gd = nullptr;
    return obj && Rd(obj, gdOffset, gd) && GameDataSid(gd, out);
}

using FnPtrArg = void (*)(void* self, void* arg);
bool CallPtrArg(void* fn, void* self, void* arg) {
    __try {
        reinterpret_cast<FnPtrArg>(fn)(self, arg);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
} // namespace

bool ReadRegionWeather(void* region, kc::RegionWeather& w) {
    void* biome = nullptr;
    void* season = nullptr;
    void* inst = nullptr;
    void* weather = nullptr;
    if (!region || !Rd(region, WR_biomeGroup, biome) || !SidAt(biome, ABG_data, w.regionSid)) return false;
    if (!Rd(region, WR_season, season) || !SidAt(season, SEASON_data, w.seasonSid)) return false;
    if (!Rd(region, WR_instance, inst) || !inst || !Rd(inst, WI_weather, weather) || !SidAt(weather, WEATHER_data, w.weatherSid)) return false;
    float dir[3];
    uint8_t ended = 0;
    bool ok = Rd(region, WR_seasonEnd, w.seasonEnd) && Rd(inst, WI_effectStrength, w.effectStrength) && Rd(inst, WI_strength, w.strength) &&
              Rd(inst, WI_windSpeed, w.windSpeed) && Rd(inst, WI_windDir, dir) && Rd(inst, WI_buEnded, ended) &&
              Rd(inst, WI_buStart, w.windBuildUpStart) && Rd(inst, WI_buEnd, w.windBuildUpEnd) && Rd(inst, WI_buSpeedStart, w.windBuildUpSpeedStart) &&
              Rd(inst, WI_buSpeedEnd, w.windBuildUpSpeedEnd) && Rd(inst, WI_buAngleStart, w.windBuildUpAngleStart) &&
              Rd(inst, WI_buAngleEnd, w.windBuildUpAngleEnd) && Rd(inst, WI_start, w.startMinutes) && Rd(inst, WI_end, w.endMinutes) &&
              Rd(inst, WI_updWind, w.updateWindMinutes) && Rd(inst, WI_time, w.time);
    w.windDir = {dir[0], dir[1], dir[2]};
    w.windBuildUpEnded = ended != 0;
    const float fl[] = {w.effectStrength, w.strength, w.windSpeed, dir[0], dir[1], dir[2], w.windBuildUpSpeedStart, w.windBuildUpSpeedEnd,
                        w.windBuildUpAngleStart, w.windBuildUpAngleEnd, w.time};
    for (float f : fl) ok = ok && std::isfinite(f);
    return ok;
}

bool WriteRegionWeather(void* region, const kc::RegionWeather& w) {
    void* inst = nullptr;
    void* curSeason = nullptr;
    void** seasonsBegin = nullptr;
    void** seasonsEnd = nullptr;
    if (!region || !Rd(region, WR_instance, inst) || !inst || !Rd(region, WR_seasonsBegin, seasonsBegin) || !Rd(region, WR_seasonsEnd, seasonsEnd))
        return false;
    // season: find it by id among the region's seasons
    std::string sid;
    if (!Rd(region, WR_season, curSeason) || !SidAt(curSeason, SEASON_data, sid) || sid != w.seasonSid) {
        const size_t n = size_t(seasonsEnd - seasonsBegin);
        if (n > 64) return false;
        bool found = false;
        for (size_t i = 0; i < n && !found; ++i) {
            void* s = nullptr;
            if (Rd(seasonsBegin, i * sizeof(void*), s) && SidAt(s, SEASON_data, sid) && sid == w.seasonSid) {
                Wr(region, WR_season, s);
                Wr(region, WR_seasonIndex, int32_t(i));
                curSeason = s;
                found = true;
            }
        }
        if (!found) return false;
    }
    Wr(region, WR_seasonEnd, w.seasonEnd);
    // weather type: find it in the season's list; switching it goes through the game's own setup
    void* curWeather = nullptr;
    if (!Rd(inst, WI_weather, curWeather) || !SidAt(curWeather, WEATHER_data, sid) || sid != w.weatherSid) {
        uint32_t count = 0;
        void** list = nullptr;
        if (!Rd(curSeason, SEASON_weatherCount, count) || !Rd(curSeason, SEASON_weathers, list) || !list || count > 256) return false;
        void* target = nullptr;
        for (uint32_t i = 0; i < count && !target; ++i) {
            void* wt = nullptr;
            if (Rd(list, i * sizeof(void*), wt) && SidAt(wt, WEATHER_data, sid) && sid == w.weatherSid) target = wt;
        }
        if (!target || !CallPtrArg(FnAddr(FnInstanceSetupWeather), inst, target)) return false;
        const uint8_t one = 1;
        Wr(region, WR_newWeather, one);     // the main-thread update replays the weather change effects
        Wr(region, WR_effectsDirty, one);
    }
    const float dir[3] = {w.windDir.x, w.windDir.y, w.windDir.z};
    const uint8_t ended = w.windBuildUpEnded ? 1 : 0;
    Wr(inst, WI_effectStrength, w.effectStrength); Wr(inst, WI_strength, w.strength); Wr(inst, WI_windSpeed, w.windSpeed);
    Wr(inst, WI_windDir, dir); Wr(inst, WI_buEnded, ended); Wr(inst, WI_buStart, w.windBuildUpStart); Wr(inst, WI_buEnd, w.windBuildUpEnd);
    Wr(inst, WI_buSpeedStart, w.windBuildUpSpeedStart); Wr(inst, WI_buSpeedEnd, w.windBuildUpSpeedEnd);
    Wr(inst, WI_buAngleStart, w.windBuildUpAngleStart); Wr(inst, WI_buAngleEnd, w.windBuildUpAngleEnd);
    Wr(inst, WI_start, w.startMinutes); Wr(inst, WI_end, w.endMinutes); Wr(inst, WI_updWind, w.updateWindMinutes); Wr(inst, WI_time, w.time);
    return true;
}

namespace {
constexpr uintptr_t CH_inventory = 0x2E8;           // Inventory*
constexpr uintptr_t INV_allItems = 0x10;            // lektor<Item*>
constexpr uintptr_t IT_manufacturer = 0xC0, IT_material = 0xC8, IT_pos = 0xDC, IT_section = 0xE8, IT_charges = 0x118,
                    IT_quality = 0x11C, IT_equipped = 0x129, IT_quantity = 0x12C;
using FnGetInv = void* (*)(const void*);
void* VirtualInventorySeh(void* fn, const void* obj) {
    __try { return reinterpret_cast<FnGetInv>(fn)(obj); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
// A character's inventory, or a building's (containers: getInventory, vtable +0x160).
void* InventoryOf(const void* obj) {
    void* inv = nullptr;
    if (!obj) return nullptr;
    if (IsCharacter(obj)) return Rd(obj, CH_inventory, inv) ? inv : nullptr;
    void* fn = VSlot(obj, 0x160);
    return fn ? VirtualInventorySeh(fn, obj) : nullptr;
}
} // namespace

namespace {
constexpr uintptr_t INV_sections = 0x28;            // boost::unordered_map<std::string, InventorySection*>
constexpr uintptr_t SEC_width = 0x30, SEC_height = 0x34, SEC_enabled = 0xD0;
constexpr uintptr_t IT_inInventory = 0xD8, IT_width = 0x130, IT_height = 0x134;
constexpr uintptr_t ITEM_getLevel = 0x2B8;           // InventoryItemBase vtable: int getLevel() const
constexpr uintptr_t INVV_addItem = 0x10, INVV_removeDontDestroy = 0x28, INVV_removeAutoDestroy = 0x30, INVV_drop = 0x38;
constexpr uintptr_t SECV_addAt = 0x18;               // InventorySection: void _addItem(Item*, int x, int y)

using FnAddItem = bool (*)(void* inv, void* item, int qty, bool dropOnFail, bool destroyOnFail);
using FnRemoveReturns = void* (*)(void* inv, void* item, int qty, bool returnCopyIfSomeLeft);
using FnRemoveDestroy = bool (*)(void* inv, void* item, int qty);
using FnItemInt = int (*)(void* item);
using FnSecAddAt = void (*)(void* sec, void* item, int x, int y);
using FnCreateItemSig = void* (*)(void* factory, void* gd, const void* hand, void* company, void* material, int level, void* uniform);

bool CallAddItem(void* fn, void* inv, void* item, int qty) {
    __try { return reinterpret_cast<FnAddItem>(fn)(inv, item, qty, false, true); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void* CallRemoveReturns(void* fn, void* inv, void* item, int qty) {
    __try { return reinterpret_cast<FnRemoveReturns>(fn)(inv, item, qty, true); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
bool CallRemoveDestroy(void* fn, void* inv, void* item, int qty) {
    __try { return reinterpret_cast<FnRemoveDestroy>(fn)(inv, item, qty); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallItemInt(void* fn, void* item, int& out) {
    __try { out = reinterpret_cast<FnItemInt>(fn)(item); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CallSecAddAt(void* fn, void* sec, void* item, int x, int y) {
    __try { reinterpret_cast<FnSecAddAt>(fn)(sec, item, x, y); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void* CallCreateItem(void* fn, void* factory, void* gd, const void* hand, void* company, void* material, int level) {
    __try { return reinterpret_cast<FnCreateItemSig>(fn)(factory, gd, hand, company, material, level, nullptr); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

constexpr uintptr_t INV_sectionList = 0x68;         // lektor<InventorySection*> sectionsInSearchOrder
constexpr uintptr_t SEC_items = 0x40;               // std::vector<SectionItem>: first, last, end
constexpr size_t kSectionItemSize = 0x10;           // SectionItem { Item*; u16 x, y, w, h; }

void ReadPointerLektor(const uint8_t* lk, std::vector<void*>& out, uint32_t max) {
    uint32_t count = 0;
    void** data = nullptr;
    if (!Rd(lk, off::LK_count, count) || count > max || !count || !Rd(lk, off::LK_data, data) || !data) return;
    for (uint32_t i = 0; i < count; ++i) {
        void* p = nullptr;
        if (Rd(data, i * sizeof(void*), p) && p) out.push_back(p);
    }
}

// Every item a character carries: the free inventory list does not hold what is worn or wielded,
// that only lives in its equipment section.
std::vector<void*> InventoryItems(void* inv) {
    std::vector<void*> out;
    ReadPointerLektor(reinterpret_cast<const uint8_t*>(inv) + INV_allItems, out, kc::kMaxItemsPerInventory);
    std::vector<void*> sections;
    ReadPointerLektor(reinterpret_cast<const uint8_t*>(inv) + INV_sectionList, sections, 64);
    for (void* sec : sections) {
        uintptr_t first = 0, last = 0;
        if (!Rd(sec, SEC_items, first) || !Rd(sec, SEC_items + 8, last) || last < first || (last - first) % kSectionItemSize ||
            (last - first) / kSectionItemSize > kc::kMaxItemsPerInventory)
            continue;
        for (uintptr_t p = first; p < last; p += kSectionItemSize) {
            void* it = nullptr;
            if (Rd(reinterpret_cast<const void*>(p), 0, it) && it && std::find(out.begin(), out.end(), it) == out.end()) out.push_back(it);
        }
        if (out.size() > kc::kMaxItemsPerInventory) break;
    }
    return out;
}

bool ReadItemState(void* it, kc::ItemState& s) {
    void* gd = nullptr;
    if (!Rd(it, off::RO_data, gd) || !GameDataSid(gd, s.templateSid)) return false;
    void* mat = nullptr;
    void* man = nullptr;
    if (Rd(it, IT_material, mat) && mat) GameDataSid(mat, s.materialSid);
    if (Rd(it, IT_manufacturer, man) && man) GameDataSid(man, s.manufacturerSid);
    ReadGameString(reinterpret_cast<uint8_t*>(it) + IT_section, s.section);
    int32_t pos[2] = {0, 0};
    uint8_t eq = 0;
    Rd(it, IT_pos, pos);
    Rd(it, IT_equipped, eq);
    Rd(it, IT_quantity, s.quantity);
    Rd(it, IT_quality, s.quality);
    Rd(it, IT_charges, s.charges);
    s.x = int16_t(pos[0]);
    s.y = int16_t(pos[1]);
    s.equipped = eq != 0;
    if (void* fn = VSlot(it, ITEM_getLevel)) CallItemInt(fn, it, s.level);
    if (!std::isfinite(s.quality)) s.quality = 0;
    if (!std::isfinite(s.charges)) s.charges = 0;
    return s.templateSid.size() <= kc::kMaxSidLen;
}

void* FindSection(void* inv, const std::string& name) {
    const auto* map = reinterpret_cast<const uint8_t*>(inv) + INV_sections;
    uint64_t size = 0, bucketCount = 0;
    void** buckets = nullptr;
    if (!Rd(map, off::US_size, size) || size == 0 || size > 256) return nullptr;
    if (!Rd(map, off::US_bucketCount, bucketCount) || !Rd(map, off::US_buckets, buckets) || !buckets) return nullptr;
    void* node = nullptr;
    if (!Rd(buckets, bucketCount * sizeof(void*), node)) return nullptr;
    std::string key;
    for (uint64_t i = 0; node && i < size; ++i) {
        void* sec = nullptr;
        if (ReadGameString(reinterpret_cast<uint8_t*>(node) + off::MapNode_key, key) && key == name && Rd(node, off::MapNode_mapped, sec)) return sec;
        if (!Rd(node, off::USNode_next, node)) break;
    }
    return nullptr;
}

// Put an item where a save load would: the named section at (x, y) when it fits, else anywhere.
using FnCanGo = bool (*)(void* sec, void* item, int x, int y);
using FnValidPos = bool (*)(void* sec, void* item, int* x, int* y);
bool CallSectionTakes(void* sec, void* item, int x, int y) {
    __try { return reinterpret_cast<FnCanGo>(FnAddr(FnSectionCanItemGoHere))(sec, item, x, y); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// The section takes this item there (type, size) and no item it holds overlaps those cells.
bool CallSectionCanGo(void* sec, void* item, int x, int y) {
    if (x < 0 || y < 0 || !CallSectionTakes(sec, item, x, y)) return false;
    int iw = 1, ih = 1;
    Rd(item, IT_width, iw);
    Rd(item, IT_height, ih);
    uintptr_t first = 0, last = 0;
    if (!Rd(sec, SEC_items, first) || !Rd(sec, SEC_items + 8, last) || last < first || (last - first) % kSectionItemSize) return false;
    for (uintptr_t p = first; p < last; p += kSectionItemSize) {
        uint16_t box[4] = {0, 0, 1, 1};   // x, y, w, h
        void* other = nullptr;
        if (!Rd(reinterpret_cast<const void*>(p), 0, other) || other == item || !Rd(reinterpret_cast<const void*>(p), 8, box)) continue;
        if (x < box[0] + box[2] && box[0] < x + iw && y < box[1] + box[3] && box[1] < y + ih) return false;
    }
    return true;
}
bool CallSectionValidPos(void* sec, void* item, int& x, int& y) {
    __try { return reinterpret_cast<FnValidPos>(FnAddr(FnSectionValidPosition))(sec, item, &x, &y); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// gameAddFallback: when the section has no free cell, let the game's Inventory::addItem find a place.
// Never for an item that must survive a failure: addItem may destroy what it cannot store.
// hostLayout: the item is where the host's game has it (a client rebuilding an inventory): a section
// our game holds disabled takes it all the same. canItemGoHere refuses anything in a disabled
// section (InventorySection +0xD0, its first test), so worn boots and shirts of NPC stand-ins went
// into the bag on clients (stress4: "aucun inventaire different", 1:44:... with 2306 boots at
// main 2,0 and 2214 shirt at main 0,2 where the host has them worn). A player's own move (the host
// replaying it) still obeys the section: hostLayout false.
bool PlaceItem(void* inv, void* item, const std::string& section, int x, int y, int qty, bool gameAddFallback = true,
               bool hostLayout = false) {
    if (void* sec = FindSection(inv, section)) {
        int w = 0, h = 0, iw = 1, ih = 1;
        uint8_t enabled = 1;
        Rd(sec, SEC_width, w); Rd(sec, SEC_height, h); Rd(item, IT_width, iw); Rd(item, IT_height, ih); Rd(sec, SEC_enabled, enabled);
        const uint8_t one = 1;
        const bool unlock = !enabled && hostLayout;   // enabled for the checks too, put back right after
        auto canGo = [&](int px, int py) {
            if (unlock) Wr(sec, SEC_enabled, one);
            const bool ok = CallSectionCanGo(sec, item, px, py);
            if (unlock) Wr(sec, SEC_enabled, enabled);
            return ok;
        };
        auto tryAt = [&](int px, int py) {
            if (px < 0 || py < 0 || px + iw > w || py + ih > h) return false;
            if (!enabled) Wr(sec, SEC_enabled, one);
            CallSecAddAt(VSlot(sec, SECV_addAt), sec, item, px, py);
            if (!enabled) Wr(sec, SEC_enabled, enabled);
            uint8_t inside = 0;
            return Rd(item, IT_inInventory, inside) && inside != 0;
        };
        if (canGo(x, y) && tryAt(x, y)) return true;
        // somewhere free in that section (the game's own "add" would equip a weapon or armour back
        // into a free slot: dragging a worn item into the bag would then snap it back on)
        for (int py = 0; py + ih <= h && py < 64; ++py)
            for (int px = 0; px + iw <= w && px < 64; ++px)
                if (canGo(px, py) && tryAt(px, py)) return true;
    }
    return gameAddFallback && CallAddItem(VSlot(inv, INVV_addItem), inv, item, qty);
}

constexpr int kItemTypeWeapon = 2;   // itemType::WEAPON

void* CreateItemFromState(const kc::ItemState& s, std::string* why = nullptr) {
    GameWorld* w = World();
    void* factory = nullptr;
    void* gd = FindGameData(s.templateSid);
    if (!gd) { if (why) *why = "unknown item " + s.templateSid; return nullptr; }
    if (!w || !Rd(w, off::GW_factory, factory) || !factory) { if (why) *why = "no factory"; return nullptr; }
    void* company = s.manufacturerSid.empty() ? nullptr : FindGameData(s.manufacturerSid);
    void* material = s.materialSid.empty() ? nullptr : FindGameData(s.materialSid);
    alignas(8) uint8_t nullHand[off::HandSize] = {};
    const uintptr_t vt = Addr(rva::VtHand);
    const uint32_t nullType = 0xB;   // what the game itself passes for "no handle yet"
    std::memcpy(nullHand, &vt, 8);
    std::memcpy(nullHand + off::H_type, &nullType, 4);
    // Weapons are made from their manufacturer: createItem(company, hand, weapon type, material, ...),
    // exactly how Character::generateWeapon calls it (their level follows from the material).
    int type = -1;
    Rd(gd, off::GD_type, type);
    void* item = nullptr;
    if (type == kItemTypeWeapon) {
        if (!company) { if (why) *why = "weapon " + s.templateSid + " without its manufacturer"; return nullptr; }
        item = CallCreateItem(FnAddr(FnCreateItem), factory, company, nullHand, gd, material, s.level);
    } else {
        item = CallCreateItem(FnAddr(FnCreateItem), factory, gd, nullHand, company, material, s.level);
    }
    if (item) Wr(item, IT_quantity, s.quantity);
    else if (why) {
        *why = "factory refused " + s.templateSid + " (type " + std::to_string(type) + ", company '" + s.manufacturerSid + "' " +
               (company ? "found" : "missing") + ", material '" + s.materialSid + "' " + (material ? "found" : "missing") + ")";
    }
    return item;
}
} // namespace

// The name of an inventory's first section (a backpack's own section: tests put things there).
std::string FirstSectionName(void* holder) {
    void* inv = InventoryOf(holder);
    if (!inv) return {};
    const auto* map = reinterpret_cast<const uint8_t*>(inv) + INV_sections;
    uint64_t size = 0, bucketCount = 0;
    void** buckets = nullptr;
    if (!Rd(map, off::US_size, size) || size == 0 || size > 256) return {};
    if (!Rd(map, off::US_bucketCount, bucketCount) || !Rd(map, off::US_buckets, buckets) || !buckets) return {};
    void* node = nullptr;
    std::string key;
    if (!Rd(buckets, bucketCount * sizeof(void*), node) || !node || !ReadGameString(reinterpret_cast<uint8_t*>(node) + off::MapNode_key, key)) return {};
    return key;
}


bool RebuildInventory(void* c, const std::vector<kc::ItemState>& items, std::string* err) {
    void* inv = InventoryOf(c);
    if (!inv) { if (err) *err = "no inventory"; return false; }
    // Keep every local item that already is exactly what the host has; only the rest changes.
    std::vector<kc::ItemState> missing = items;
    std::vector<void*> extra;
    bool extraEquipped = false;
    for (void* it : InventoryItems(inv)) {
        kc::ItemState s;
        if (!ReadItemState(it, s)) continue;
        auto m = std::find(missing.begin(), missing.end(), s);
        if (m != missing.end()) { missing.erase(m); continue; }
        extra.push_back(it);
        extraEquipped |= s.equipped;
    }
    if (extraEquipped && IsCharacter(c)) EndCombat(static_cast<Character*>(c));   // it may be holding the weapon that goes away
    for (void* it : extra) {
        int q = 1;
        Rd(it, IT_quantity, q);
        CallRemoveDestroy(VSlot(inv, INVV_removeAutoDestroy), inv, it, q);
    }
    bool ok = true;
    for (const auto& s : missing) {
        std::string why;
        void* item = CreateItemFromState(s, &why);
        if (!item) { ok = false; if (err) *err = why; continue; }
        if (!PlaceItem(inv, item, s.section, s.x, s.y, s.quantity, true, true)) { ok = false; if (err) *err = "cannot place " + s.templateSid; }
    }
    return ok;
}

namespace {
struct Stack { void* item = nullptr; int qty = 0; };
void ItemPlace(void* it, std::string& sec, int& x, int& y) {
    int32_t pos[2] = {0, 0};
    Rd(it, IT_pos, pos);
    ReadGameString(reinterpret_cast<uint8_t*>(it) + IT_section, sec);
    x = pos[0];
    y = pos[1];
}
bool ItemKind(void* it, kc::ItemState& s) {
    void* gd = nullptr;
    if (!Rd(it, off::RO_data, gd) || !GameDataSid(gd, s.templateSid)) return false;
    void* mat = nullptr;
    void* man = nullptr;
    if (Rd(it, IT_material, mat) && mat) GameDataSid(mat, s.materialSid);
    if (Rd(it, IT_manufacturer, man) && man) GameDataSid(man, s.manufacturerSid);
    if (void* fn = VSlot(it, ITEM_getLevel)) CallItemInt(fn, it, s.level);
    return true;
}
// the stack the client moved: same kind, at least that many, preferably at the same place
Stack FindStack(void* inv, const kc::ItemState& want) {
    Stack best;
    for (void* it : InventoryItems(inv)) {
        kc::ItemState s;
        if (!ItemKind(it, s) || !s.sameKind(want)) continue;
        int q = 0, x = 0, y = 0;
        std::string sec;
        Rd(it, IT_quantity, q);
        ItemPlace(it, sec, x, y);
        const bool samePlace = sec == want.section && x == want.x && y == want.y;
        if (q >= want.quantity && (!best.item || samePlace)) { best = {it, q}; if (samePlace) break; }
    }
    return best;
}
// the item lying at that place (its top-left cell) of an inventory, other than `except`
void* ItemAt(void* inv, const std::string& section, int x, int y, void* except) {
    for (void* it : InventoryItems(inv)) {
        std::string sec;
        int ix = 0, iy = 0;
        ItemPlace(it, sec, ix, iy);
        if (it != except && sec == section && ix == x && iy == y) return it;
    }
    return nullptr;
}
} // namespace

bool MoveInventoryItem(void* from, void* to, const kc::InvOp& op, std::string* err) {
    void* src = InventoryOf(from);
    void* dst = InventoryOf(to);
    if (!src || !dst) { if (err) *err = "no inventory"; return false; }
    const Stack found = FindStack(src, op.item);
    void* best = found.item;
    const int bestQty = found.qty;
    if (!best) { if (err) *err = "item not found"; return false; }
    if (op.kind == kc::InvOpKind::Drop) {
        // a chest's item: dropped by the player's character standing by it (`to`), as the game does
        // with the chest's user; a character's: its inventory drops it, as the inventory window does
        if (!IsCharacter(from) && IsCharacter(to)) return DropFromHolder(from, best, static_cast<Character*>(to));
        void* fn = VSlot(src, INVV_drop);
        return fn && CallPtrArg2(fn, src, best);
    }
    const int qty = std::min(op.item.quantity, bestQty);
    // dropped on a stack of the same kind: it joins that stack, as the client's game showed it (only
    // for what surely stacks, a pile of two or more: two single swords trade places instead)
    if (void* onto = ItemAt(dst, op.toSection, op.toX, op.toY, best)) {
        kc::ItemState k;
        int have = 0;
        if (ItemKind(onto, k) && k.sameKind(op.item) && Rd(onto, IT_quantity, have) && have > 0 && (have > 1 || qty > 1)) {
            CallRemoveDestroy(VSlot(src, INVV_removeAutoDestroy), src, best, qty);
            const int sum = have + qty;
            Wr(onto, IT_quantity, sum);
            if (err) err->clear();
            return true;
        }
    }
    void* moving = CallRemoveReturns(VSlot(src, INVV_removeDontDestroy), src, best, qty);
    if (!moving) { if (err) *err = "cannot take the item"; return false; }
    // Only cells we checked are free: the game's addItem could destroy the item when it finds no
    // room, and putting a destroyed item back would leave a dangling pointer in the inventory.
    if (PlaceItem(dst, moving, op.toSection, op.toX, op.toY, qty, false)) return true;
    // never lose it: back where it was (that cell was just freed)
    if (!PlaceItem(src, moving, op.item.section, op.item.x, op.item.y, qty, false)) PlaceItem(src, moving, op.item.section, op.item.x, op.item.y, qty);
    if (err) *err = "no room";
    return false;
}

bool SwapInventoryItems(void* from, void* to, const kc::InvOp& a, const kc::InvOp& b, std::string* err) {
    void* ia = InventoryOf(from);
    void* ib = InventoryOf(to);
    if (!ia || !ib) { if (err) *err = "no inventory"; return false; }
    const Stack sa = FindStack(ia, a.item);
    const Stack sb = FindStack(ib, b.item);
    // whole stacks only, and really where the client saw them (else it is not this swap)
    if (!sa.item || !sb.item || sa.item == sb.item || sa.qty != a.item.quantity || sb.qty != b.item.quantity) { if (err) *err = "not the same items here"; return false; }
    std::string sec;
    int x = 0, y = 0;
    ItemPlace(sb.item, sec, x, y);
    if (sec != a.toSection || x != a.toX || y != a.toY) { if (err) *err = "the slot holds something else here"; return false; }
    void* ma = CallRemoveReturns(VSlot(ia, INVV_removeDontDestroy), ia, sa.item, sa.qty);
    if (!ma) { if (err) *err = "cannot take the first item"; return false; }
    void* mb = CallRemoveReturns(VSlot(ib, INVV_removeDontDestroy), ib, sb.item, sb.qty);
    if (!mb) {
        if (!PlaceItem(ia, ma, a.item.section, a.item.x, a.item.y, sa.qty, false)) PlaceItem(ia, ma, a.item.section, a.item.x, a.item.y, sa.qty);
        if (err) *err = "cannot take the second item";
        return false;
    }
    const bool okA = PlaceItem(ib, ma, a.toSection, a.toX, a.toY, sa.qty, false);
    if (!okA) {   // both back home, as they were
        if (!PlaceItem(ib, mb, b.item.section, b.item.x, b.item.y, sb.qty, false)) PlaceItem(ib, mb, b.item.section, b.item.x, b.item.y, sb.qty);
        if (!PlaceItem(ia, ma, a.item.section, a.item.x, a.item.y, sa.qty, false)) PlaceItem(ia, ma, a.item.section, a.item.x, a.item.y, sa.qty);
        if (err) *err = "no room for the first item";
        return false;
    }
    // the second where the client put it, else anywhere it fits in that inventory, never lost
    if (!PlaceItem(ia, mb, b.toSection, b.toX, b.toY, sb.qty, false) && !PlaceItem(ia, mb, a.item.section, a.item.x, a.item.y, sb.qty, false))
        PlaceItem(ia, mb, b.toSection, b.toX, b.toY, sb.qty);
    return true;
}

namespace {
using FnShowFlag = int (*)(void*);
using FnCallbackChar = void* (*)(void*);
using FnStealNotice = bool (*)(void*, void*, void*);
using FnTheftFrom = void (*)(void*, void*);
using FnSetCrimeSig = bool (*)(void*, int, void*, const void*);
int NumWindowsSeh() {
    __try { return reinterpret_cast<FnShowFlag>(Addr(0x6E2DF0))(reinterpret_cast<void*>(Addr(rva::TradeGui))); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
bool CloseAllSeh() {
    __try { reinterpret_cast<void (*)(void*)>(Addr(0x6E5740))(reinterpret_cast<void*>(Addr(rva::TradeGui))); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void* CallbackCharSeh(void* inv) {
    __try { return reinterpret_cast<FnCallbackChar>(Addr(0x70CEF0))(inv); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
bool NoticeSeh(void* fn, void* thief, void* from, void* item, bool& caught) {
    __try { caught = reinterpret_cast<FnStealNotice>(fn)(thief, from, item); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool TheftFromSeh(void* fn, void* item, void* owner) {
    __try { reinterpret_cast<FnTheftFrom>(fn)(item, owner); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SetCrimeSeh(void* bm, int crime, void* faction, const void* hand) {
    __try { return reinterpret_cast<FnSetCrimeSig>(FnAddr(FnSetCrime))(bm, crime, faction, hand); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
} // namespace

int OpenInventoryWindows() { return NumWindowsSeh(); }
bool CloseInventoryWindows() { return CloseAllSeh(); }

void* FindItemIn(void* container, const kc::ItemState& want) {
    void* inv = InventoryOf(container);
    if (!inv) return nullptr;
    void* best = nullptr;
    for (void* it : InventoryItems(inv)) {
        kc::ItemState s;
        if (!ReadItemState(it, s) || !s.sameKind(want)) continue;
        if (s.section == want.section && s.x == want.x && s.y == want.y) return it;
        if (!best) best = it;
    }
    return best;
}

int StealCheck(Character* thief, void* container, void* item) {
    void* inv = InventoryOf(container);
    if (!IsCharacter(thief) || !inv || !item) return 0;
    // the victim: the container's owner (for furniture, its building's resident squad leader)
    void* victim = CallbackCharSeh(inv);
    if (!IsCharacter(victim)) return 0;
    void* faction = nullptr;
    if (void* fn = VSlot(victim, 0x58)) faction = CallNoArgPtrOn(fn, victim);   // getFaction
    void* ourFaction = nullptr;
    if (PlayerInterface* pi = Player()) Rd(pi, 0x2A0, ourFaction);
    if (!faction || faction == ourFaction) return 0;
    uint8_t notReal = 0;
    if (Rd(faction, 0x1D0, notReal) && notReal) return 0;   // Faction::notARealFaction
    // the game's own steps, as its loot window does them: the crime, then the roll to be seen
    SetCrimeSeh(reinterpret_cast<uint8_t*>(thief) + 0xF0, 3, faction, reinterpret_cast<uint8_t*>(victim) + off::RO_handle);   // THEFT
    bool caught = false;
    if (void* fn = VSlot(thief, 0x298); fn && NoticeSeh(fn, thief, container, item, caught) && caught) return 2;   // ImStealingDoYouNotice
    void* owner = nullptr;
    Rd(inv, 0x88, owner);   // Inventory::owner
    if (void* fn = VSlot(item, 0x348)) TheftFromSeh(fn, item, owner);   // Item::notifyTheftFrom
    return 1;
}

// ---------------------------------------------------------------- trade with merchants
namespace {
constexpr uintptr_t OW_home = 0x38;          // Ownerships: hand of the home building (type 0xB: none)
constexpr uintptr_t OW_cats = 0x88;          // Ownerships: money
constexpr uintptr_t BU_interior = 0x1F0;     // Building::myInterior
constexpr int kTradeForMoney = 1;            // TradeWindowType::TW_MONEY_TRADING
// std::map node of the open trade windows: left, parent, right, key (InventoryGUI*) at +0x18,
// InventoryTradeData at +0x20 (+0xA isPlayer), isNil at +0x51
constexpr uintptr_t TP_left = 0x0, TP_right = 0x10, TP_key = 0x18, TP_isPlayer = 0x2A, TP_isNil = 0x51;
constexpr uintptr_t GUIV_getInventory = 0x70;   // InventoryGUI vtable: Inventory* getInventory()

struct ShopLektor {   // the game's lektor<Building*>
    uintptr_t vt;
    uint32_t count, capacity;
    void** data;
};
using FnOwnershipsSig = void* (*)(void* c);
using FnCollectSig = void (*)(void* interior, ShopLektor* out);
using FnNewSig = void* (*)(size_t);
using FnDeleteSig = void (*)(void*);
using FnTakeMoneySig = bool (*)(void* c, int amount);
using FnNpcTraderSig = void* (*)();
using FnRClickSig = int* (*)(void* window, int* out, const void* section, int x, int y, void* to, bool thievery, bool first);

void* OwnershipsSeh(void* c) {
    __try { return reinterpret_cast<FnOwnershipsSig>(FnAddr(FnGetOwnerships))(c); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
bool CollectSeh(void* interior, ShopLektor* out) {
    __try { reinterpret_cast<FnCollectSig>(FnAddr(FnInteriorShopFurniture))(interior, out); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void* GameNewSeh(size_t n) {
    __try { return reinterpret_cast<FnNewSig>(Addr(rva::GameNew))(n); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
void GameDeleteSeh(void* p) {
    __try { reinterpret_cast<FnDeleteSig>(Addr(rva::GameDelete))(p); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
bool TakeMoneySeh(void* c, int amount, bool& ok) {
    __try { ok = reinterpret_cast<FnTakeMoneySig>(FnAddr(FnCharTakeMoney))(c, amount); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void* NpcTraderSeh() {
    __try { return reinterpret_cast<FnNpcTraderSig>(FnAddr(FnGetNpcTrader))(); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
bool RClickSeh(void* window, int* out, const void* section, int x, int y, void* to) {
    __try { reinterpret_cast<FnRClickSig>(FnAddr(FnRClickAutoTrade))(window, out, section, x, y, to, true, true); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// The windows of the open trade (merchant's side, player's side), from the game's partner map.
bool TradeWindows(void*& merchant, void*& player) {
    merchant = player = nullptr;
    void* head = nullptr;
    uint64_t size = 0;
    if (!Rd(reinterpret_cast<void*>(Addr(rva::TradePartnersHead)), 0, head) || !head ||
        !Rd(reinterpret_cast<void*>(Addr(rva::TradePartnersSize)), 0, size) || size < 2 || size > 16)
        return false;
    void* root = nullptr;
    if (!Rd(head, 0x8, root) || !root) return false;   // head->parent is the root
    std::vector<void*> todo{root};
    for (int guard = 0; !todo.empty() && guard < 64; ++guard) {
        void* n = todo.back();
        todo.pop_back();
        uint8_t nil = 1;
        if (!n || !Rd(n, TP_isNil, nil) || nil) continue;
        void* window = nullptr;
        uint8_t isPlayer = 0;
        if (Rd(n, TP_key, window) && window && Rd(n, TP_isPlayer, isPlayer)) (isPlayer ? player : merchant) = window;
        void* l = nullptr;
        void* r = nullptr;
        if (Rd(n, TP_left, l)) todo.push_back(l);
        if (Rd(n, TP_right, r)) todo.push_back(r);
    }
    return merchant && player;
}
} // namespace

bool ShopCounters(Character* trader, std::vector<void*>& out) {
    out.clear();
    if (!IsCharacter(trader)) return false;
    void* own = OwnershipsSeh(trader);
    kc::Handle home;
    if (!own || !ReadHandle(reinterpret_cast<uint8_t*>(own) + OW_home, home) || home.type != 0 || !home.valid()) return false;
    void* building = ResolveObject(home);
    void* interior = nullptr;
    if (!building || !Rd(building, BU_interior, interior) || !interior) return false;
    // what the game's own trade window collects (ShopTrader's constructor), into a list of the
    // game's own kind (its memory comes from the game's allocator: the game may grow it)
    constexpr uint32_t kCap = 256;
    ShopLektor l{Addr(rva::VtLektor), 0, kCap, static_cast<void**>(GameNewSeh(kCap * sizeof(void*)))};
    if (!l.data) return false;
    const bool ok = CollectSeh(interior, &l);
    for (uint32_t i = 0; ok && i < l.count && i < 1024; ++i) {
        void* f = nullptr;
        if (Rd(l.data, i * sizeof(void*), f) && f && InventoryOf(f)) out.push_back(f);
    }
    GameDeleteSeh(l.data);
    return !out.empty();
}

bool HasHomeBuilding(Character* trader) {
    // as ShopTrader's constructor (0x953850) decides: a home hand that is not "none" (0xB) and
    // resolves to an object; otherwise it sells from its squad's worn backpacks
    void* own = IsCharacter(trader) ? OwnershipsSeh(trader) : nullptr;
    kc::Handle home;
    return own && ReadHandle(reinterpret_cast<uint8_t*>(own) + OW_home, home) && home.type != 0xB && home.valid() && ResolveObject(home);
}

// ShopTrader's constructor (0x953850) reads the merchant's home from getOwnerships(merchant)+0x38
// (an NPC's ownerships are its squad's) and builds the window from that building's interior. A
// client's merchant can be a stand-in the mod made for the host's (its own was generated apart and
// had another handle): it has no home, and the window showed nothing while the counters held the
// host's stock. It is given the building the counters belong to, which also covers a home that
// differs here.
int GiveShopHome(Character* trader, const std::vector<void*>& counters) {
    if (!IsCharacter(trader) || counters.empty()) return -1;
    auto covered = [&](const std::vector<void*>& have) {
        size_t n = 0;
        for (void* c : counters) n += std::find(have.begin(), have.end(), c) != have.end();
        return n;
    };
    std::vector<void*> now;
    if (ShopCounters(trader, now) && covered(now) == counters.size()) return 0;
    // the buildings the counters are furniture of; the one whose shop furniture holds most of them
    std::vector<void*> parents;
    for (void* c : counters)
        if (void* p = FurnitureParent(c); p && std::find(parents.begin(), parents.end(), p) == parents.end()) parents.push_back(p);
    void* best = nullptr;
    size_t bestN = 0;
    constexpr uint32_t kCap = 256;
    for (void* b : parents) {
        void* interior = nullptr;
        if (!Rd(b, BU_interior, interior) || !interior) continue;
        ShopLektor l{Addr(rva::VtLektor), 0, kCap, static_cast<void**>(GameNewSeh(kCap * sizeof(void*)))};
        if (!l.data) continue;
        std::vector<void*> got;
        if (CollectSeh(interior, &l))
            for (uint32_t i = 0; i < l.count && i < 1024; ++i)
                if (void* f = nullptr; Rd(l.data, i * sizeof(void*), f) && f) got.push_back(f);
        GameDeleteSeh(l.data);
        if (const size_t n = covered(got); n > bestN) { best = b; bestN = n; }
    }
    kc::Handle home;
    void* own = OwnershipsSeh(trader);
    if (!best || !own || !ObjectHandle(best, home) || home.type != 0) return -1;
    // the hand's fields only (its vtable stays)
    auto* hand = reinterpret_cast<uint8_t*>(own) + OW_home;
    if (!Wr(hand, off::H_type, home.type) || !Wr(hand, off::H_container, home.container) ||
        !Wr(hand, off::H_containerSerial, home.containerSerial) || !Wr(hand, off::H_index, home.index) || !Wr(hand, off::H_serial, home.serial))
        return -1;
    return ShopCounters(trader, now) && covered(now) > 0 ? 1 : -1;
}

bool FloorFocus(kc::Handle& out) {
    PlayerInterface* pi = Player();
    return pi && ReadHandle(reinterpret_cast<const uint8_t*>(pi) + off::PI_focusHand, out) && out.type != 0xB && out.valid();
}

bool SetFloorFocus(Character* c) {
    PlayerInterface* pi = Player();
    kc::Handle want, cur;
    if (!pi || !IsCharacter(c) || !GetHandle(c, want)) return false;
    auto* hand = reinterpret_cast<uint8_t*>(pi) + off::PI_focusHand;
    if (ReadHandle(hand, cur) && cur == want) return true;
    // the hand's fields as 0x7F5400 copies them (its vtable stays); the last floor seen set to none,
    // so the next update shows the floor this character is on, as 0x7F5400 does
    return Wr(hand, off::H_type, want.type) && Wr(hand, off::H_container, want.container) && Wr(hand, off::H_containerSerial, want.containerSerial) &&
           Wr(hand, off::H_index, want.index) && Wr(hand, off::H_serial, want.serial) && Wr(pi, off::PI_focusFloor, int32_t(-1));
}

namespace {
constexpr uintptr_t SEC_type = 0xB8;      // InventorySection: its kind (Inventory::getSection(type) 0x745EF0 compares it)
constexpr int kSectionBackpack = 12;      // the worn backpack's attach section
constexpr uintptr_t AP_members = 0x50;    // ActivePlatoon: lektor<Character*> of its members (0x953850 reads +0x58/+0x60)
constexpr size_t kMaxWornInSection = 16;
} // namespace

// The backpack a character wears, as ShopTrader's constructor finds it: the first item of its
// inventory section of kind 12, when that item has an inventory of its own (RootObject::getInventory,
// vt 0x160: 0 for a plain Item (0xD2280), ContainerItem::getInventory (0x76BE60) returns +0x290).
void* WornBackpack(Character* c) {
    if (!IsCharacter(c)) return nullptr;
    void* inv = InventoryOf(c);
    if (!inv) return nullptr;
    std::vector<void*> sections;
    ReadPointerLektor(reinterpret_cast<const uint8_t*>(inv) + INV_sectionList, sections, 64);
    for (void* sec : sections) {
        int32_t type = -1;
        if (!Rd(sec, SEC_type, type) || type != kSectionBackpack) continue;
        uintptr_t first = 0, last = 0;
        if (!Rd(sec, SEC_items, first) || !Rd(sec, SEC_items + 8, last) || last <= first || (last - first) % kSectionItemSize ||
            (last - first) / kSectionItemSize > kMaxWornInSection)
            continue;
        void* item = nullptr;
        if (!Rd(reinterpret_cast<const void*>(first), 0, item) || !item || IsCharacter(item)) continue;
        void* bagInv = InventoryOf(item);
        if (!bagInv) continue;
        std::vector<void*> inner;
        ReadPointerLektor(reinterpret_cast<const uint8_t*>(bagInv) + INV_sectionList, inner, 64);
        if (!inner.empty()) return item;
    }
    return nullptr;
}

bool HasInventory(const void* obj) { return InventoryOf(obj) != nullptr; }

bool TravellingWearers(Character* trader, std::vector<Character*>& out) {
    out.clear();
    if (!IsCharacter(trader) || HasHomeBuilding(trader)) return false;
    void* squad = SquadOf(trader);
    if (!squad) return false;
    std::vector<void*> members;
    ReadPointerLektor(reinterpret_cast<const uint8_t*>(squad) + AP_members, members, 256);
    for (void* m : members)
        if (IsCharacter(m) && WornBackpack(static_cast<Character*>(m))) out.push_back(static_cast<Character*>(m));
    return !out.empty();
}

bool IsAnimal(const void* obj) { return obj && Vtable(obj) == Addr(rva::VtCharacterAnimal); }

bool MoneyOf(Character* c, int32_t& out) {
    void* own = IsCharacter(c) ? OwnershipsSeh(c) : nullptr;
    return own && Rd(own, OW_cats, out);
}

bool SetMoneyOf(Character* c, int32_t money) {
    void* own = IsCharacter(c) ? OwnershipsSeh(c) : nullptr;
    return own && Wr(own, OW_cats, money);
}

bool TakeMoney(Character* c, int32_t amount) {
    bool ok = false;
    return IsCharacter(c) && TakeMoneySeh(c, amount, ok) && ok;
}

bool OpenTradeWindow(Character* looter, Character* trader) {
    if (!IsCharacter(looter) || !IsCharacter(trader)) return false;
    // stored by the game, opened by its GUI on the next update (like the loot window)
    const auto* a = reinterpret_cast<const uint8_t*>(looter) + off::RO_handle;
    const auto* b = reinterpret_cast<const uint8_t*>(trader) + off::RO_handle;
    return CallShowTrade(FnAddr(FnShowTradeWindow), reinterpret_cast<void*>(Addr(rva::TradeGui)), a, b, kTradeForMoney);
}

Character* NpcTrader() {
    void* c = NpcTraderSeh();
    return IsCharacter(c) ? static_cast<Character*>(c) : nullptr;
}

bool ReadOperatorCount(void* useable, uint64_t& n) {
    constexpr uintptr_t US_operatorCount = 0x3E0;   // UseableStuff: std::set<hand> of operators (+0x3D0), size
    // only UseableStuff has the set: furniture with a function other than none / fluff
    const int f = BuildingFunctionOf(useable);
    return f > 0 && f != 18 && Rd(useable, US_operatorCount, n) && n < 64;
}

int BuildingFunctionOf(void* building) {
    void* fn = building && !IsCharacter(building) ? VSlot(building, 0x2F0) : nullptr;   // Building::getSpecialFunction
    int v = -1;
    return fn && CallItemInt(fn, building, v) ? v : -1;
}

bool MouseHoldsItem() {
    void* mouse = nullptr;
    void* item = nullptr;
    return Rd(reinterpret_cast<void*>(Addr(rva::MouseInventory)), 0, mouse) && mouse && Rd(mouse, 0x30, item) && item;
}

bool InventoryWindowShows(const void* obj) {
    if (!obj) return false;
    void* head = nullptr;
    uint64_t size = 0;
    if (!Rd(reinterpret_cast<void*>(Addr(rva::TradePartnersHead)), 0, head) || !head ||
        !Rd(reinterpret_cast<void*>(Addr(rva::TradePartnersSize)), 0, size) || size == 0 || size > 16)
        return false;
    if (NpcTrader() == obj) return true;
    void* root = nullptr;
    if (!Rd(head, 0x8, root) || !root) return false;
    std::vector<void*> todo{root};
    for (int guard = 0; !todo.empty() && guard < 64; ++guard) {
        void* n = todo.back();
        todo.pop_back();
        uint8_t nil = 1;
        if (!n || !Rd(n, TP_isNil, nil) || nil) continue;
        void* window = nullptr;
        if (Rd(n, TP_key, window) && window) {
            // InventoryGUI vtable: +0x78 getCallbackCharacter, +0x80 getCallbackObject
            for (uintptr_t slot : {uintptr_t(0x78), uintptr_t(0x80)})
                if (void* fn = VSlot(window, slot); fn && CallNoArgPtrOn(fn, window) == obj) return true;
        }
        void* l = nullptr;
        void* r = nullptr;
        if (Rd(n, TP_left, l)) todo.push_back(l);
        if (Rd(n, TP_right, r)) todo.push_back(r);
    }
    return false;
}

bool TradeWindowItems(bool merchantSide, std::vector<WindowItem>& out) {
    out.clear();
    void* merchant = nullptr;
    void* player = nullptr;
    if (!TradeWindows(merchant, player)) return false;
    void* window = merchantSide ? merchant : player;
    void* fn = VSlot(window, GUIV_getInventory);
    void* inv = fn ? CallNoArgPtrOn(fn, window) : nullptr;
    if (!inv) return false;
    // every section of that inventory (by name) and the items laid out in it
    const auto* map = reinterpret_cast<const uint8_t*>(inv) + INV_sections;
    uint64_t size = 0, bucketCount = 0;
    void** buckets = nullptr;
    if (!Rd(map, off::US_size, size) || size == 0 || size > 256) return true;
    if (!Rd(map, off::US_bucketCount, bucketCount) || !Rd(map, off::US_buckets, buckets) || !buckets) return true;
    void* node = nullptr;
    if (!Rd(buckets, bucketCount * sizeof(void*), node)) return true;
    for (uint64_t i = 0; node && i < size; ++i) {
        std::string name;
        void* sec = nullptr;
        if (ReadGameString(reinterpret_cast<uint8_t*>(node) + off::MapNode_key, name) && Rd(node, off::MapNode_mapped, sec) && sec) {
            uintptr_t first = 0, last = 0;
            if (Rd(sec, SEC_items, first) && Rd(sec, SEC_items + 8, last) && last >= first && (last - first) % kSectionItemSize == 0 &&
                (last - first) / kSectionItemSize < 2000) {
                for (uintptr_t p = first; p < last; p += kSectionItemSize) {
                    void* it = nullptr;
                    uint16_t box[2] = {0, 0};
                    WindowItem w;
                    if (!Rd(reinterpret_cast<const void*>(p), 0, it) || !it || !Rd(reinterpret_cast<const void*>(p), 8, box) || !ReadItemState(it, w.state)) continue;
                    w.section = name;
                    w.x = box[0];
                    w.y = box[1];
                    out.push_back(std::move(w));
                }
            }
        }
        if (!Rd(node, off::USNode_next, node)) break;
    }
    return true;
}

int TradeRightClick(bool merchantSide, const WindowItem& item) {
    void* merchant = nullptr;
    void* player = nullptr;
    if (!TradeWindows(merchant, player)) return -1;
    alignas(8) uint8_t section[kGameStringSize];
    GameStringView(item.section, section);
    int result = -1;
    if (!RClickSeh(merchantSide ? merchant : player, &result, section, item.x, item.y, merchantSide ? player : merchant)) return -1;
    return result;
}

bool ReadInventory(const void* c, std::vector<kc::ItemState>& out) {
    out.clear();
    void* inv = InventoryOf(c);
    if (!inv) return false;
    for (void* it : InventoryItems(inv)) {
        kc::ItemState s;
        if (ReadItemState(it, s)) out.push_back(std::move(s));
    }
    if (out.size() > kc::kMaxItemsPerInventory) out.resize(kc::kMaxItemsPerInventory);
    std::sort(out.begin(), out.end(), [](const kc::ItemState& a, const kc::ItemState& b) {
        return std::tie(a.section, a.y, a.x, a.templateSid, a.quantity) < std::tie(b.section, b.y, b.x, b.templateSid, b.quantity);
    });
    return true;
}

bool ExpireRegionWeather(void* region) {
    void* inst = nullptr;
    const int32_t zero = 0;
    return region && Rd(region, WR_instance, inst) && inst && Wr(inst, WI_end, zero);
}

namespace {
// WeatherRegion: vector<EffectGroup*>
constexpr uintptr_t WR_effectsBegin = 0x70, WR_effectsEnd = 0x78;
// EffectGroup: effect GameData, vector<EffectHandler*>, countdown to the next spawn
constexpr uintptr_t EG_data = 0x8, EG_handlersBegin = 0x20, EG_handlersEnd = 0x28, EG_spawnTimer = 0x40;
constexpr uintptr_t EG_vSpawn = 0x20;   // bool spawn()
// EffectHandler
constexpr uintptr_t EH_pos = 0x24, EH_strength = 0x48, EH_age = 0x54, EH_life = 0x58, EH_endless = 0x5C;
constexpr uintptr_t EH_vStop = 0x20;    // void stop()
constexpr uintptr_t EHP_struck = 0x69, EHP_strikeIn = 0x6C;                  // EffectHandlerPoint (lightning)
constexpr uintptr_t EHW_dir = 0x68, EHW_turnTo = 0x74, EHW_turnIn = 0x80;    // EffectHandlerWandering
constexpr float kNeverTurn = 1e9f;      // clients' wandering effects turn only when the host's do

thread_local const float* t_effectPos = nullptr;

bool ReadPointerVector(const void* obj, uintptr_t beginOff, uintptr_t endOff, size_t max, std::vector<void*>& out) {
    out.clear();
    void** b = nullptr;
    void** e = nullptr;
    if (!Rd(obj, beginOff, b) || !Rd(obj, endOff, e) || e < b || size_t(e - b) > max) return false;
    out.resize(size_t(e - b));
    return out.empty() || SafeCopy(out.data(), b, out.size() * sizeof(void*));
}

bool HandlerOfKind(const void* h, kc::EffectKind kind) {
    return Vtable(h) == Addr(kind == kc::EffectKind::Point ? rva::VtEffectHandlerPoint : rva::VtEffectHandlerWandering);
}

bool RdVec(const void* p, uintptr_t offset, kc::Vec3& v) {
    float f[3];
    if (!Rd(p, offset, f)) return false;
    v = {f[0], f[1], f[2]};
    return Finite(v);
}
bool WrVec(void* p, uintptr_t offset, const kc::Vec3& v) {
    const float f[3] = {v.x, v.y, v.z};
    return Finite(v) && Wr(p, offset, f);
}
} // namespace

bool ReadEffectGroups(void* region, std::vector<EffectGroupInfo>& out) {
    out.clear();
    std::vector<void*> groups;
    if (!region || !ReadPointerVector(region, WR_effectsBegin, WR_effectsEnd, 64, groups)) return false;
    std::vector<std::pair<kc::EffectKind, std::string>> counted;
    for (void* g : groups) {
        const uintptr_t vt = Vtable(g);
        EffectGroupInfo gi;
        if (vt == Addr(rva::VtEffectGroupPoint)) gi.kind = kc::EffectKind::Point;
        else if (vt == Addr(rva::VtEffectGroupWandering)) gi.kind = kc::EffectKind::Wandering;
        else continue;
        if (!SidAt(g, EG_data, gi.effectSid) || !ReadPointerVector(g, EG_handlersBegin, EG_handlersEnd, 1024, gi.handlers)) continue;
        gi.group = g;
        size_t same = 0;
        for (const auto& c : counted) same += c.first == gi.kind && c.second == gi.effectSid ? 1 : 0;
        if (same > 255) continue;
        gi.ordinal = uint8_t(same);
        counted.emplace_back(gi.kind, gi.effectSid);
        out.push_back(std::move(gi));
    }
    return true;
}

bool ReadEffect(void* h, kc::EffectKind kind, kc::WeatherEffect& e) {
    if (!HandlerOfKind(h, kind)) return false;
    uint8_t endless = 0;
    bool ok = RdVec(h, EH_pos, e.pos) && Rd(h, EH_strength, e.strength) && Rd(h, EH_age, e.age) && Rd(h, EH_life, e.life) &&
              Rd(h, EH_endless, endless);
    e.endless = endless != 0;
    if (kind == kc::EffectKind::Point) {
        uint8_t struck = 0;
        ok = ok && Rd(h, EHP_struck, struck) && Rd(h, EHP_strikeIn, e.strikeIn);
        e.struck = struck != 0;
    } else {
        ok = ok && RdVec(h, EHW_dir, e.dir) && RdVec(h, EHW_turnTo, e.turnTo);
    }
    return ok && std::isfinite(e.strength) && std::isfinite(e.age) && std::isfinite(e.life) && std::isfinite(e.strikeIn);
}

void BlockEffectSpawns(void* group) {
    const float later = 1e6f;
    Wr(group, EG_spawnTimer, later);
}

void* SpawnEffect(void* group, const kc::WeatherEffect& e) {
    void* fn = VSlot(group, EG_vSpawn);
    if (!fn || !Finite(e.pos)) return nullptr;
    std::vector<void*> before, after;
    if (!ReadPointerVector(group, EG_handlersBegin, EG_handlersEnd, 1024, before)) return nullptr;
    const float pos[3] = {e.pos.x, e.pos.y, e.pos.z};
    bool spawned = false;
    t_effectPos = pos;
    const bool called = CallBool(fn, group, spawned);
    t_effectPos = nullptr;
    if (!called || !spawned || !ReadPointerVector(group, EG_handlersBegin, EG_handlersEnd, 1024, after) || after.size() != before.size() + 1)
        return nullptr;
    void* h = after.back();
    if (!HandlerOfKind(h, e.kind)) return nullptr;
    // the host's random rolls instead of ours
    const uint8_t endless = e.endless ? 1 : 0;
    Wr(h, EH_strength, e.strength); Wr(h, EH_age, e.age); Wr(h, EH_life, e.life); Wr(h, EH_endless, endless);
    if (e.kind == kc::EffectKind::Point) {
        const uint8_t struck = e.struck ? 1 : 0;
        Wr(h, EHP_struck, struck);
        Wr(h, EHP_strikeIn, e.strikeIn);
    } else {
        WrVec(h, EHW_dir, e.dir);
        WrVec(h, EHW_turnTo, e.turnTo);
        Wr(h, EHW_turnIn, kNeverTurn);
    }
    return h;
}

bool WriteEffectState(void* h, const kc::EffectState& s) {
    if (!HandlerOfKind(h, kc::EffectKind::Wandering)) return false;
    return WrVec(h, EH_pos, s.pos) && WrVec(h, EHW_dir, s.dir) && WrVec(h, EHW_turnTo, s.turnTo) && Wr(h, EHW_turnIn, kNeverTurn);
}

bool StopEffect(void* h) {
    if (!HandlerOfKind(h, kc::EffectKind::Point) && !HandlerOfKind(h, kc::EffectKind::Wandering)) return false;
    void* fn = VSlot(h, EH_vStop);
    return fn && CallVoid(fn, h);
}

const float* EffectSpawnPosition() { return t_effectPos; }

bool RegionRebuildingEffects(void* region) {
    uint8_t dirty = 0;
    return Rd(region, WR_effectsDirty, dirty) && dirty != 0;
}

bool EffectShown(void* h) {
    uint8_t shown = 0;
    return Rd(h, 0x38, shown) && shown != 0;
}

void HurryEffectSpawn(void* group) {
    const float now = 0.0f;
    Wr(group, EG_spawnTimer, now);
}

std::string DescribeRegionWeathers(void* region) {
    constexpr uintptr_t WEATHER_effectsBegin = 0x10, WEATHER_effectsEnd = 0x18, kEffectEntry = 0x18;   // pair<GameData*, TripleInt>
    auto label = [](void* gd) {
        std::string sid, name;
        GameDataSid(gd, sid);
        ReadGameString(reinterpret_cast<const uint8_t*>(gd) + off::GD_name, name);
        return sid + " '" + name + "'";
    };
    std::vector<void*> seasons;
    if (!region || !ReadPointerVector(region, WR_seasonsBegin, WR_seasonsEnd, 64, seasons)) return "";
    std::string out;
    for (void* season : seasons) {
        uint32_t count = 0;
        void** list = nullptr;
        void* sgd = nullptr;
        if (!Rd(season, SEASON_data, sgd) || !Rd(season, SEASON_weatherCount, count) || !Rd(season, SEASON_weathers, list) || !list || count > 256)
            continue;
        std::string ssid;
        GameDataSid(sgd, ssid);
        for (uint32_t i = 0; i < count; ++i) {
            void* wt = nullptr;
            void* gd = nullptr;
            uint8_t* b = nullptr;
            uint8_t* e = nullptr;
            if (!Rd(list, i * sizeof(void*), wt) || !Rd(wt, WEATHER_data, gd)) continue;
            out += "  weather " + label(gd) + " season " + ssid + " effects:";
            if (Rd(wt, WEATHER_effectsBegin, b) && Rd(wt, WEATHER_effectsEnd, e) && e >= b && size_t(e - b) <= 64 * kEffectEntry)
                for (uint8_t* q = b; q < e; q += kEffectEntry) {
                    void* fx = nullptr;
                    if (Rd(q, 0, fx)) out += " " + label(fx);
                }
            out += "\n";
        }
    }
    return out;
}

void* CameraWeatherRegion() {
    void* system = nullptr;
    void* region = nullptr;
    return Rd(reinterpret_cast<void*>(Addr(rva::WeatherSystem)), 0, system) && system && Rd(system, 0, region) ? region : nullptr;
}

bool SetRagdoll(Character* c, bool on) {
    return IsCharacter(c) && CallBoolInt(FnAddr(FnRagdollMode), c, on, kRagdollWhole);
}

bool StandUp(Character* c) {
    void* m = Medical(c);
    if (!m) return false;
    const bool no = false;
    const float zero = 0.0f;
    Wr(m, off::MS_unconscious, no);
    Wr(m, off::MS_koTimer, zero);
    bool ok = SetRagdoll(c, false);
    if (void* fn = VSlot(c, slot::CH_setProneState)) ok = CallInt(fn, c, kProneNormal) && ok;
    return ok;
}

bool DestroyObject(void* obj) {
    GameWorld* w = World();
    if (!w || !IsCharacter(obj)) return false;
    // a character of the player's squads has a portrait in the squad bar (PortraitData, an item of a
    // tab's ItemBox): the game only takes it out through its squad (ActivePlatoon), never by a
    // straight delete; the mod never deletes one
    if (InPlayerSquad(static_cast<Character*>(obj))) return false;
    return CallDestroy(FnAddr(FnWorldDestroy), w, obj);
}

float GetFrameSpeed() {
    float v = 1.0f;
    GameWorld* w = World();
    return (w && Rd(w, off::GW_frameSpeedMult, v) && std::isfinite(v)) ? v : 1.0f;
}

bool GetPaused() {
    bool v = false;
    GameWorld* w = World();
    return w && Rd(w, off::GW_paused, v) && v;
}

bool Teleport(Character* c, const kc::Vec3& pos, const kc::Quat& rot) {
    void* m = Movement(c);
    if (!m || !Finite(pos)) return false;
    void* fn = VSlot(m, slot::CM_setPositionDirectionAndTeleport);
    const float p[3] = {pos.x, pos.y, pos.z};
    const float q[4] = {rot.w, rot.x, rot.y, rot.z};
    return fn && CallTeleport(fn, m, p, q);
}

namespace {
constexpr uintptr_t CM_speedOrders = 0x20, CM_desiredSpeed = 0xBC;   // CharMovement: MoveSpeed, float
constexpr int32_t kMoveSpeedCount = 4;
} // namespace

bool GetFacing(const Character* c, kc::Vec3& dir) {
    constexpr uintptr_t CM_facing = 0xD0;
    void* m = Movement(c);
    float d[3];
    if (!m || !Rd(m, CM_facing, d)) return false;
    dir = {d[0], d[1], d[2]};
    return Finite(dir);
}

bool FaceDirection(Character* c, const kc::Vec3& dir) {
    constexpr uintptr_t CM_vFaceDirection = 0x30;
    void* m = Movement(c);
    void* fn = m ? VSlot(m, CM_vFaceDirection) : nullptr;
    const float d[3] = {dir.x, dir.y, dir.z};
    return fn && Finite(dir) && CallPtrArg2(fn, m, const_cast<float*>(d));
}

void* MovementOf(const Character* c) { return Movement(c); }

kc::Vec3 ForwardOf(const kc::Quat& q) {   // q * (0,0,1)
    return {2 * (q.x * q.z + q.w * q.y), 2 * (q.y * q.z - q.w * q.x), 1 - 2 * (q.x * q.x + q.y * q.y)};
}

bool ReadPace(const Character* c, uint8_t& gait, float& pace) {
    void* m = Movement(c);
    int32_t order = 0;
    if (!m || !Rd(m, CM_speedOrders, order) || !Rd(m, CM_desiredSpeed, pace) || !std::isfinite(pace)) return false;
    gait = uint8_t(order >= 0 && order < kMoveSpeedCount ? order : 0);
    return true;
}

bool WritePace(Character* c, uint8_t gait, float pace) {
    void* m = Movement(c);
    if (!m || gait >= kMoveSpeedCount || !std::isfinite(pace) || pace < 0) return false;
    float cur = 0;
    int32_t order = 0;
    if (Rd(m, CM_desiredSpeed, cur) && Rd(m, CM_speedOrders, order) && cur == pace && order == gait) return true;
    return Wr(m, CM_speedOrders, int32_t(gait)) && Wr(m, CM_desiredSpeed, pace);
}

namespace {
constexpr uintptr_t CH_animation = 0x448;      // Character: AnimationClass*
constexpr uintptr_t AC_owner = 0x2D8;          // AnimationClass: Character* me
constexpr uintptr_t AC_animList = 0x2C0;       // AnimationClass: AnimsListsManager::AnimList*
constexpr uintptr_t AL_allAnims = 0xB8;        // AnimList: unordered_map<string, AnimationData*>
constexpr uintptr_t AC_reqAction = 0x210, AC_reqCarryL = 0x21E, AC_reqCarryR = 0x21F, AC_reqCarried = 0x220,
                    AC_reqCombatMode = 0x244;  // AnimationRequirement (at +0xF0) fields
constexpr uintptr_t AD_dataName = 0x8;         // AnimationData
constexpr uintptr_t TQ_animation = 0x0;        // CombatTechniqueData
constexpr uintptr_t kTechniqueList = 0x2011F78;   // lektor<CombatTechniqueData*>: every technique

using FnCombatAnim = void (*)(void* ac, void* technique, float speed, void* extra);
using FnPlayAction = void (*)(void* ac, void* anim, float speedMult, float weight, bool stumble);
using FnAnimPtr = void (*)(void* ac, void* anim);
using FnAnimBool3 = void (*)(void* ac, bool a, bool b, bool c);
using FnAnimStr = bool (*)(void* ac, const void* name);
bool SehCombatAnim(void* fn, void* ac, void* t, float speed, void* extra) {
    __try { reinterpret_cast<FnCombatAnim>(fn)(ac, t, speed, extra); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SehPlayAction(void* fn, void* ac, void* a, float s, float w, bool st) {
    __try { reinterpret_cast<FnPlayAction>(fn)(ac, a, s, w, st); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SehAnimPtr(void* fn, void* ac, void* a) {
    __try { reinterpret_cast<FnAnimPtr>(fn)(ac, a); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SehAnimBool3(void* fn, void* ac, bool a, bool b, bool c) {
    __try { reinterpret_cast<FnAnimBool3>(fn)(ac, a, b, c); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SehAnimStr(void* fn, void* ac, const void* name) {
    __try { reinterpret_cast<FnAnimStr>(fn)(ac, name); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
} // namespace

bool ReadStdString(const void* p, std::string& out) { return ReadGameString(p, out); }

Character* AnimOwner(const void* ac) {
    void* c = nullptr;
    return ac && Rd(ac, AC_owner, c) && IsCharacter(c) ? static_cast<Character*>(c) : nullptr;
}

void* AnimationOf(const Character* c) {
    void* ac = nullptr;
    return IsCharacter(c) && Rd(c, CH_animation, ac) && AnimOwner(ac) == c ? ac : nullptr;
}

std::string TechniqueName(const void* t) {
    std::string s;
    if (t) ReadGameString(reinterpret_cast<const uint8_t*>(t) + TQ_animation, s);
    return s;
}

std::string AnimDataName(const void* a) {
    std::string s;
    if (a) ReadGameString(reinterpret_cast<const uint8_t*>(a) + AD_dataName, s);
    return s;
}

void* FindTechnique(const std::string& name) {
    static std::unordered_map<std::string, void*> cache;
    if (name.empty()) return nullptr;
    if (auto it = cache.find(name); it != cache.end()) return it->second;
    // attacks, then blocks (and dodges), each a lektor<CombatTechniqueData*> right after the other
    for (uintptr_t listRva : {kTechniqueList, kTechniqueList + 0x18, kTechniqueList + 0x30, kTechniqueList - 0x18}) {
        const void* list = reinterpret_cast<const void*>(Addr(listRva));
        uint32_t n = 0;
        void** data = nullptr;
        if (!Rd(list, off::LK_count, n) || !Rd(list, off::LK_data, data) || !data || n > 100000) continue;
        for (uint32_t i = 0; i < n; ++i) {
            void* t = nullptr;
            if (Rd(data, i * sizeof(void*), t) && t && TechniqueName(t) == name) {
                cache[name] = t;
                return t;
            }
        }
    }
    return nullptr;
}

void* FindAnimData(const Character* c, const std::string& name) {
    void* ac = AnimationOf(c);
    void* list = nullptr;
    if (!ac || name.empty() || !Rd(ac, AC_animList, list) || !list) return nullptr;
    const uint8_t* map = reinterpret_cast<const uint8_t*>(list) + AL_allAnims;
    uint64_t size = 0, bucketCount = 0;
    void** buckets = nullptr;
    void* node = nullptr;
    if (!Rd(map, off::US_size, size) || size == 0 || size > 100000 || !Rd(map, off::US_bucketCount, bucketCount) ||
        !Rd(map, off::US_buckets, buckets) || !buckets || !Rd(buckets, bucketCount * sizeof(void*), node))
        return nullptr;
    for (uint64_t i = 0; node && i < size; ++i) {
        void* a = nullptr;
        if (Rd(node, off::MapNode_mapped, a) && a && AnimDataName(a) == name) return a;
        if (!Rd(node, off::USNode_next, node)) break;
    }
    return nullptr;
}

bool ReadAnimModes(const Character* c, AnimModes& out) {
    void* ac = AnimationOf(c);
    void* action = nullptr;
    int32_t combat = 0;
    uint8_t l = 0, r = 0, carried = 0;
    if (!ac || !Rd(ac, AC_reqAction, action) || !Rd(ac, AC_reqCombatMode, combat) || !Rd(ac, AC_reqCarryL, l) || !Rd(ac, AC_reqCarryR, r) ||
        !Rd(ac, AC_reqCarried, carried))
        return false;
    out.action = AnimDataName(action);
    out.combat = combat == 1;
    out.carried = carried != 0;
    out.carryLeft = l != 0;
    out.carryRight = r != 0;
    uint8_t gl = 0, gu = 0;
    Rd(ac, 0x24C, gl);
    Rd(ac, 0x24D, gu);
    out.guardLegs = gl != 0;
    out.guardUpper = gu != 0;
    return true;
}

std::string CurrentTechniqueName(const Character* c) {
    constexpr uintptr_t AC_reqTechnique = 0x208;
    void* ac = AnimationOf(c);
    void* t = nullptr;
    std::string n = ac && Rd(ac, AC_reqTechnique, t) && t ? TechniqueName(t) : "";
    for (char& ch : n) if (ch == ' ') ch = '_';
    return n.empty() ? "-" : n;
}

std::string ItemTemplate(const void* item);

namespace {
constexpr uintptr_t AC_layers = 0xD0;                       // lektor<AnimationLayer*>
constexpr uintptr_t AL_addList = 0x0, AL_removeList = 0x18;  // lektor<SingleAnimation*>
constexpr uintptr_t SA_name = 0x0, SA_data = 0x30, SA_speed = 0x40, SA_weight = 0x44, SA_desired = 0x48, SA_time = 0x50, SA_time01 = 0x54,
                    SA_looped = 0x5D;
} // namespace

bool ReadPlayingAnims(const Character* c, std::vector<PlayingAnim>& out) {
    out.clear();
    void* ac = AnimationOf(c);
    if (!ac) return false;
    uint32_t nl = 0;
    void** layers = nullptr;
    if (!Rd(ac, AC_layers + off::LK_count, nl) || !Rd(ac, AC_layers + off::LK_data, layers) || !layers || nl > 8) return false;
    for (uint32_t li = 0; li < nl; ++li) {
        void* layer = nullptr;
        if (!Rd(layers, li * sizeof(void*), layer) || !layer) continue;
        for (int list = 0; list < 2; ++list) {
            const uintptr_t lo = list == 0 ? AL_addList : AL_removeList;
            uint32_t n = 0;
            void** items = nullptr;
            if (!Rd(layer, lo + off::LK_count, n) || !Rd(layer, lo + off::LK_data, items) || !items || n > 64) continue;
            for (uint32_t i = 0; i < n; ++i) {
                void* sa = nullptr;
                PlayingAnim a;
                uint8_t looped = 0;
                void* data = nullptr;
                if (!Rd(items, i * sizeof(void*), sa) || !sa || !ReadGameString(sa, a.anim) || !Rd(sa, SA_speed, a.speed) || !Rd(sa, SA_weight, a.weight) ||
                    !Rd(sa, SA_desired, a.desired) || !Rd(sa, SA_time, a.time) || !Rd(sa, SA_time01, a.time01) || !Rd(sa, SA_looped, looped))
                    continue;
                if (Rd(sa, SA_data, data) && data) a.data = AnimDataName(data);
                uint8_t synched = 0;
                Rd(sa, 0x5E, synched);
                a.synched = synched != 0;
                a.layer = uint8_t(li);
                a.looped = looped != 0;
                a.fadingOut = list == 1;
                a.single = sa;
                out.push_back(std::move(a));
            }
        }
    }
    return true;
}

bool ReadAnimMaster(const Character* c, float& time, float& speed) {
    constexpr uintptr_t AC_masterTime = 0xC8, AC_masterSpeed = 0xCC;
    void* ac = AnimationOf(c);
    return ac && Rd(ac, AC_masterTime, time) && Rd(ac, AC_masterSpeed, speed) && std::isfinite(time) && std::isfinite(speed);
}

bool WriteAnimMaster(void* ac, float time, float speed) {
    constexpr uintptr_t AC_masterTime = 0xC8, AC_masterSpeed = 0xCC;
    return ac && std::isfinite(time) && std::isfinite(speed) && Wr(ac, AC_masterTime, time) && Wr(ac, AC_masterSpeed, speed);
}

void* SingleAnimOwner(const void* single) {
    constexpr uintptr_t SA_class = 0x38;
    void* ac = nullptr;
    return single && Rd(single, SA_class, ac) ? ac : nullptr;
}

bool SingleAnimName(const void* single, std::string& out) { return single && ReadGameString(single, out); }

bool ReadSingleAnimTime(const void* sa, float& time) { return sa && Rd(sa, SA_time, time) && std::isfinite(time); }

bool ReadAnimMasterOf(const void* ac, float& time, float& speed) {
    constexpr uintptr_t AC_masterTime = 0xC8, AC_masterSpeed = 0xCC;
    return ac && Rd(ac, AC_masterTime, time) && Rd(ac, AC_masterSpeed, speed) && std::isfinite(time) && std::isfinite(speed);
}

float SyncedAnimTime(const void* sa, float mine, float want, bool looped, float gameSpeed) {
    // closer than that, our own smooth progress stays (the host's samples arrive with network
    // jitter, which the game speed multiplies)
    const float kTolerance = std::min(0.4f, 0.12f * std::max(1.0f, gameSpeed));
    float d = want - mine;
    float t01 = 0;
    if (looped && Rd(sa, SA_time01, t01) && t01 > 0.02f && mine > 0.01f) {
        const float length = mine / t01;
        if (std::isfinite(length) && length > 0.05f) {
            d = std::fmod(d, length);
            if (d > length * 0.5f) d -= length;
            if (d < -length * 0.5f) d += length;
        }
    }
    // far off: jump to the host's; a little off: ease toward it (no visible step); close: ours
    if (std::fabs(d) > kTolerance * 2.5f) return mine + d;
    if (std::fabs(d) > kTolerance * 0.25f) return mine + d * 0.15f;
    return mine;
}

void WriteSingleAnim(void* sa, float time, float speed, float weight, float desired) {
    constexpr uintptr_t SA_stillWanted = 0x67;
    if (!sa || !std::isfinite(time) || !std::isfinite(speed)) return;
    const uint8_t wanted = desired > 0.0f ? 1 : 0;   // the per-frame selection we skip would set it
    Wr(sa, SA_stillWanted, wanted);
    Wr(sa, SA_time, time);
    Wr(sa, SA_speed, speed);
    Wr(sa, SA_weight, weight);
    Wr(sa, SA_desired, desired);
}

namespace {
using FnRunAnimLayer = void (*)(void* ac, void* anim, float speed, int layer, float blend);
bool SehRunAnimLayer(void* fn, void* ac, void* a, float s, int l, float b) {
    __try { reinterpret_cast<FnRunAnimLayer>(fn)(ac, a, s, l, b); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
} // namespace

bool CallRunAnimation(Character* c, void* animData, float speed, int layer, float blend) {
    void* ac = AnimationOf(c);
    return ac && animData && SehRunAnimLayer(FnAddr(FnRunAnimationLayer), ac, animData, speed, layer, blend);
}

std::string CurrentStumbleName(const Character* c) {
    constexpr uintptr_t AC_reqStumble = 0x228;
    void* ac = AnimationOf(c);
    void* a = nullptr;
    std::string n = ac && Rd(ac, AC_reqStumble, a) && a ? AnimDataName(a) : "";
    return n.empty() ? "-" : n;
}

bool WeaponInHands(const Character* c, std::string& itemSid, std::string& fromSection) {
    constexpr uintptr_t CHH_weaponInHands = 0x6D8, CHH_sheathLocation = 0x6E0;
    void* w = nullptr;
    if (Vtable(c) != Addr(rva::VtCharacterHuman) || !Rd(c, CHH_weaponInHands, w) || !w) return false;
    itemSid = ItemTemplate(w);
    ReadGameString(reinterpret_cast<const uint8_t*>(c) + CHH_sheathLocation, fromSection);
    return !itemSid.empty();
}

bool CallStartCombatAnim(void* fn, Character* c, void* technique, float speed) {
    void* ac = AnimationOf(c);
    GameString extra;
    if (!ac || !technique || !fn || !MakeGameString("", extra)) return false;
    return SehCombatAnim(fn, ac, technique, speed, &extra);   // the callee owns (and destroys) the by-value string
}

bool CallAnimVoid(void* fn, Character* c) {
    void* ac = AnimationOf(c);
    return ac && fn && CallVoid(fn, ac);
}

bool CallPlayAction(Character* c, void* animData, float speedMult, float weight, bool stumble) {
    void* ac = AnimationOf(c);
    return ac && animData && SehPlayAction(FnAddr(FnAnimPlayAction), ac, animData, speedMult, weight, stumble);
}

bool CallStopActionNamed(Character* c, const std::string& name) {
    void* ac = AnimationOf(c);
    if (!ac) return false;
    if (name.empty()) return CallVoid(FnAddr(FnAnimStopAction), ac);
    GameString s;
    if (!MakeGameString(name, s)) return CallVoid(FnAddr(FnAnimStopAction), ac);
    return SehAnimStr(FnAddr(FnAnimStopActionNamed), ac, &s);
}

bool CallStartStumble(Character* c, void* animData) {
    void* ac = AnimationOf(c);
    return ac && animData && SehAnimPtr(FnAddr(FnAnimStartStumble), ac, animData);
}

bool CallSetCombatMode(Character* c, bool on) {
    void* ac = AnimationOf(c);
    return ac && CallBoolArg(FnAddr(FnAnimSetCombatMode), ac, on);
}

bool CallSetGuard(Character* c, bool legs, bool on) {
    void* ac = AnimationOf(c);
    return ac && CallBoolArg(FnAddr(legs ? FnAnimGuardLegs : FnAnimGuardUpper), ac, on);
}

Character* CharacterOfHand(const void* hand) {
    void* fn = FnAddr(FnHandleResolve);
    void* table = reinterpret_cast<void*>(Addr(rva::HandleTable));
    void* obj = hand ? CallResolve(fn, table, hand) : nullptr;
    return IsCharacter(obj) ? static_cast<Character*>(obj) : nullptr;
}

namespace {
using FnCreateLabel = void* (*)(void* gui, const void* text, const float* colour, int size, int speed);
using FnLabelTrack = void (*)(void* label, const void* hand, const float* offset);
using FnLabelColor = void (*)(void* label, const float* colour);
void* SehCreateLabel(void* fn, void* gui, const void* t, const float* c, int sz, int sp) {
    __try { return reinterpret_cast<FnCreateLabel>(fn)(gui, t, c, sz, sp); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
bool SehLabelTrack(void* fn, void* l, const void* h, const float* o) {
    __try { reinterpret_cast<FnLabelTrack>(fn)(l, h, o); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SehLabelColor(void* fn, void* l, const float* c) {
    __try { reinterpret_cast<FnLabelColor>(fn)(l, c); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
} // namespace

void* ShowFloater(Character* c, const std::string& text, const float colour[4], int size, int speed) {
    GameString s;
    if (!IsCharacter(c) || !MakeGameString(text.substr(0, 15), s)) return nullptr;
    void* label = SehCreateLabel(FnAddr(FnCreateScreenLabel), reinterpret_cast<void*>(Addr(rva::TradeGui)), &s, colour, size, speed);
    if (!label) return nullptr;
    const float offset[3] = {0.0f, 15.0f, 0.0f};   // as addWound places it
    SehLabelTrack(FnAddr(FnLabelSetTracking), label, reinterpret_cast<const uint8_t*>(c) + off::RO_handle, offset);
    return label;
}

namespace {
constexpr uintptr_t IT_inInventoryFlag = 0xD8, IT_itemGroup = 0x188;
constexpr uintptr_t IV_isPhysical = 0xF8, IV_activate = 0x228, IV_setInventoryWeAreIn = 0x358, IV_getPosition = 0x40;
constexpr uintptr_t kNoHandleGlobal = 0x1E3A5F8;      // the empty hand the game gives a dropped item
constexpr uintptr_t kIdentityQuatPtr = 0x2247DA0;     // pointer to Quaternion::IDENTITY, as dropItem uses it
using FnActivate = void (*)(void* item, bool on, const float* pos, const float* quat, bool a, const int* indoors, bool b);
bool SehActivate(void* fn, void* item, const float* pos, const float* quat) {
    const int maybe = 2;
    __try { reinterpret_cast<FnActivate>(fn)(item, true, pos, quat, false, &maybe, false); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
} // namespace

bool ItemOnGround(void* item) {
    uint8_t inInv = 1;
    void* group = nullptr;
    bool physical = false;
    void* fn = VSlot(item, IV_isPhysical);
    return item && Rd(item, IT_inInventoryFlag, inInv) && inInv == 0 && Rd(item, IT_itemGroup, group) && !group && fn &&
           CallBool(fn, item, physical) && physical;
}

bool ItemLoose(void* item) {
    uint8_t inInv = 1;
    return item && !IsCharacter(item) && Rd(item, IT_inInventoryFlag, inInv) && inInv == 0;
}

bool DescribeGroundItem(void* item, kc::Handle& h, kc::ItemState& s, kc::Vec3& pos) {
    float p[3];
    void* fn = VSlot(item, IV_getPosition);
    if (!item || !ReadHandle(reinterpret_cast<uint8_t*>(item) + off::RO_handle, h) || !ReadItemState(item, s) || !fn || !CallGetVec3(fn, item, p))
        return false;
    pos = {p[0], p[1], p[2]};
    s.section.clear();
    s.x = s.y = 0;
    s.equipped = false;
    return Finite(pos);
}

void* ResolveItem(const kc::Handle& h) {
    alignas(8) uint8_t hand[off::HandSize] = {};
    const uintptr_t vt = Addr(rva::VtHand);
    std::memcpy(hand, &vt, 8);
    std::memcpy(hand + off::H_type, &h.type, 4);
    std::memcpy(hand + off::H_container, &h.container, 4);
    std::memcpy(hand + off::H_containerSerial, &h.containerSerial, 4);
    std::memcpy(hand + off::H_index, &h.index, 4);
    std::memcpy(hand + off::H_serial, &h.serial, 4);
    void* obj = CallResolve(FnAddr(FnHandleResolve), reinterpret_cast<void*>(Addr(rva::HandleTable)), hand);
    kc::Handle back;
    return obj && ReadHandle(reinterpret_cast<uint8_t*>(obj) + off::RO_handle, back) && back == h && !IsCharacter(obj) ? obj : nullptr;
}

void* CreateGroundItem(const kc::ItemState& s, const kc::Vec3& pos, kc::Handle& localHandle, std::string* why) {
    void* item = CreateItemFromState(s, why);
    if (!item) return nullptr;
    void* setInv = VSlot(item, IV_setInventoryWeAreIn);
    void* activate = VSlot(item, IV_activate);
    void* quat = nullptr;
    if (!setInv || !activate || !Rd(reinterpret_cast<void*>(Addr(kIdentityQuatPtr)), 0, quat) || !quat) return nullptr;
    CallPtrArg2(setInv, item, reinterpret_cast<void*>(Addr(kNoHandleGlobal)));
    const float p[3] = {pos.x, pos.y, pos.z};
    if (!SehActivate(activate, item, p, static_cast<const float*>(quat))) return nullptr;
    ReadHandle(reinterpret_cast<uint8_t*>(item) + off::RO_handle, localHandle);
    return item;
}

void* FirstLooseItem(Character* c) {
    void* inv = InventoryOf(c);
    if (!inv) return nullptr;
    for (void* it : InventoryItems(inv)) {
        uint8_t equipped = 0;
        if (Rd(it, IT_equipped, equipped) && !equipped) return it;
    }
    return nullptr;
}

bool CallDropItem(Character* c, void* item) {
    constexpr uintptr_t CH_vDropItem = 0x1A8;
    void* fn = VSlot(c, CH_vDropItem);
    return IsCharacter(c) && item && fn && CallPtrArg2(fn, c, item);
}

bool CallGiveItem(Character* c, void* item) {
    using FnGive = bool (*)(void*, void*, bool, bool);
    return IsCharacter(c) && item && reinterpret_cast<FnGive>(FnAddr(FnGiveItem))(c, item, false, false);
}

bool GiveNewItem(Character* c, const std::string& sid, int32_t qty, std::string* why) {
    if (!IsCharacter(c)) { if (why) *why = "no character"; return false; }
    kc::ItemState st;
    st.templateSid = sid;
    st.quantity = qty > 0 ? qty : 1;
    void* item = CreateItemFromState(st, why);
    if (!item) return false;
    if (!CallGiveItem(c, item)) { if (why) *why = "giveItem refused (inventory full?)"; return false; }
    return true;
}

namespace {
constexpr uintptr_t kZoneManagerPtr = 0x21349C0;   // GameWorld::zoneMgr, as AI::findFoodOnGround reads it
constexpr uintptr_t ZM_objectGrid = 0x80;          // ZoneSpacialGrid of objects
constexpr uintptr_t kFnGetObjects = 0x9FF210;      // ZoneSpacialGrid::getObjects(point, radius, filter, lektor&, max)
constexpr uintptr_t kLektorPtrVt = 0x168BAF0;      // lektor<RootObject*> vtable
constexpr uintptr_t kGameNew = 0xED6504, kGameDelete = 0xED64FE;   // the game's operator new / delete
struct GameLektor {
    uintptr_t vt;
    uint32_t count, capacity;
    void** data;
};
using FnGetObjects = int (*)(void* grid, const float* pt, float radius, int filter, GameLektor* out, int max);
using FnNew = void* (*)(size_t);
using FnDelete = void (*)(void*);
bool SehGetObjects(void* grid, const float* pt, float r, GameLektor* lk) {
    __try { reinterpret_cast<FnGetObjects>(Addr(kFnGetObjects))(grid, pt, r, 0, lk, 0x7FFFFFFF); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
} // namespace

bool DescribeInventoryItem(void* item, kc::ItemState& s) { return item && ReadItemState(item, s); }

void AllObjectsNear(const kc::Vec3& pos, float radius, std::vector<void*>& out) {
    out.clear();
    void* zm = nullptr;
    if (!Rd(reinterpret_cast<void*>(Addr(kZoneManagerPtr)), 0, zm) || !zm) return;
    // the zone manager keeps one grid per kind: buildings and furniture (beds, stools, chests,
    // machines) at +0x48, items at +0x80 (characters at +0x10, not wanted here)
    for (uintptr_t grid : {uintptr_t(0x48), ZM_objectGrid}) {
        GameLektor lk{Addr(kLektorPtrVt), 0, 10, nullptr};
        lk.data = static_cast<void**>(reinterpret_cast<FnNew>(Addr(kGameNew))(10 * sizeof(void*)));
        if (!lk.data) return;
        const float pt[3] = {pos.x, pos.y, pos.z};
        if (SehGetObjects(reinterpret_cast<uint8_t*>(zm) + grid, pt, radius, &lk))
            for (uint32_t i = 0; i < lk.count && i < 100000; ++i) {
                void* o = nullptr;
                if (Rd(lk.data, i * sizeof(void*), o) && o) out.push_back(o);
            }
        if (lk.data) reinterpret_cast<FnDelete>(Addr(kGameDelete))(lk.data);
    }
}

namespace {
void ItemsNearIf(const kc::Vec3& pos, float radius, std::vector<void*>& out, bool (*keep)(void*));
} // namespace

void GroundItemsNear(const kc::Vec3& pos, float radius, std::vector<void*>& out) {
    ItemsNearIf(pos, radius, out, [](void* o) { return !IsCharacter(o) && ItemOnGround(o); });
}

void LooseItemsNear(const kc::Vec3& pos, float radius, std::vector<void*>& out) {
    ItemsNearIf(pos, radius, out, [](void* o) { return ItemLoose(o); });
}

namespace {
void ItemsNearIf(const kc::Vec3& pos, float radius, std::vector<void*>& out, bool (*keep)(void*)) {
    out.clear();
    void* zm = nullptr;
    if (!Rd(reinterpret_cast<void*>(Addr(kZoneManagerPtr)), 0, zm) || !zm) return;
    // a lektor exactly as the game builds one for this call (its storage from the game's heap)
    GameLektor lk{Addr(kLektorPtrVt), 0, 10, nullptr};
    lk.data = static_cast<void**>(reinterpret_cast<FnNew>(Addr(kGameNew))(10 * sizeof(void*)));
    if (!lk.data) return;
    const float pt[3] = {pos.x, pos.y, pos.z};
    if (SehGetObjects(reinterpret_cast<uint8_t*>(zm) + ZM_objectGrid, pt, radius, &lk))
        for (uint32_t i = 0; i < lk.count && i < 100000; ++i) {
            void* o = nullptr;
            if (Rd(lk.data, i * sizeof(void*), o) && o && keep(o)) out.push_back(o);
        }
    if (lk.data) reinterpret_cast<FnDelete>(Addr(kGameDelete))(lk.data);
}
} // namespace

namespace {
// Every class whose activate is Item::activate (vt 0x228 -> 0x75D9B0): Item and its subclasses.
constexpr uintptr_t kItemVtables[] = {0x1685358 /* SeveredLimbItem */, 0x16857C8 /* Gear */, 0x1685F78 /* RobotLimbItem */,
                                      0x16B61F8 /* BlueprintItem */, 0x170DD18 /* Item */, 0x170E138 /* MapItem */,
                                      0x170E9F8 /* NestItem */, 0x170EF28 /* MoneyItem */, 0x170F4F8 /* ContainerItem */,
                                      0x1723B18 /* Weapon */, 0x1724398 /* Armour */, 0x17247F8 /* LockedArmour */,
                                      0x1726CB8 /* Sword */, 0x17270F8 /* Crossbow */};
constexpr uintptr_t IT_active = 0x190;            // set by activate (vt 0x230), cleared by deactivate
constexpr int kItemTypeArmour = 3;                // itemType::ARMOUR
} // namespace

bool IsItemObject(const void* obj) {
    const uintptr_t vt = obj ? Vtable(obj) : 0;
    if (!vt) return false;
    for (uintptr_t v : kItemVtables)
        if (vt == Addr(v)) return true;
    return false;
}

bool ItemInWorld(void* item) {
    uint8_t inInv = 1, active = 0;
    void* group = nullptr;
    return IsItemObject(item) && Rd(item, IT_inInventoryFlag, inInv) && inInv == 0 && Rd(item, IT_active, active) && active != 0 &&
           Rd(item, IT_itemGroup, group) && !group;
}

void WorldItemsNear(const kc::Vec3& pos, float radius, std::vector<void*>& out) {
    ItemsNearIf(pos, radius, out, [](void* o) { return ItemInWorld(o); });
}

void* InventoryCallback(void* inventory) {
    void* cb = nullptr;
    return inventory && Rd(inventory, 0x80, cb) ? cb : nullptr;
}

void* InventoryOfHolder(void* holder) { return InventoryOf(holder); }

bool InventoryDrop(void* inventory, void* item) {
    void* fn = inventory && item ? VSlot(inventory, INVV_drop) : nullptr;
    return fn && CallPtrArg2(fn, inventory, item);
}

bool InventoryRemove(void* inventory, void* item) {
    void* fn = inventory && item ? VSlot(inventory, INVV_removeDontDestroy) : nullptr;
    using FnRemove = bool (*)(void* inv, void* item, int qty, bool returnCopyIfSomeLeft);
    __try { return fn && reinterpret_cast<FnRemove>(fn)(inventory, item, -1, true); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool DropFromHolder(void* holder, void* item, Character* dropper) {
    void* inv = InventoryOf(holder);
    if (!inv || !item || !IsCharacter(dropper)) return false;
    // out of the chest first (the dropper's dropItem would otherwise look for it in its own body's
    // sections), whole stack, never destroyed
    if (!InventoryRemove(inv, item)) return false;
    return CallDropItem(dropper, item);
}

void* FindItemOfKind(void* holder, const std::string& kind) {
    void* inv = InventoryOf(holder);
    if (!inv) return nullptr;
    void* worn = nullptr;
    for (void* it : InventoryItems(inv)) {
        void* gd = nullptr;
        int type = -1;
        std::string sid, name;
        if (!Rd(it, off::RO_data, gd) || !gd || !GameDataSid(gd, sid)) continue;
        Rd(gd, off::GD_type, type);
        bool match = false;
        if (kind == "weapon") match = type == kItemTypeWeapon;
        else if (kind == "armour") match = type == kItemTypeArmour;
        else if (kind == "item") match = type != kItemTypeWeapon && type != kItemTypeArmour;
        else {
            std::string part = kind;
            std::replace(part.begin(), part.end(), '_', ' ');
            match = sid == kind || (TemplateDisplayName(sid, name) && name.find(part) != std::string::npos);
        }
        if (!match) continue;
        uint8_t equipped = 0;
        Rd(it, IT_equipped, equipped);
        if (!equipped) return it;
        if (!worn) worn = it;
    }
    return worn;
}

bool DestroyItem(void* item) {
    constexpr uintptr_t IV_deactivate = 0x238;   // out of the world first, as a pickup does
    GameWorld* w = World();
    if (!w || !item || IsCharacter(item)) return false;
    if (void* fn = VSlot(item, IV_deactivate)) CallVoid(fn, item);
    return CallDestroy(FnAddr(FnWorldDestroy), w, item);
}

bool SetLabelColor(void* label, const float colour[4]) { return label && SehLabelColor(FnAddr(FnLabelSetColor), label, colour); }

bool CallSetCarryMode(Character* c, bool carried, bool left, bool right) {
    void* ac = AnimationOf(c);
    return ac && SehAnimBool3(FnAddr(FnAnimSetCarryMode), ac, carried, left, right);
}

namespace {
using FnDraw = bool (*)(void* c, void* item, void* section);
bool SehDraw(void* fn, void* c, void* item, void* section) {
    __try { return reinterpret_cast<FnDraw>(fn)(c, item, section); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
} // namespace

std::string ItemTemplate(const void* item) {
    void* gd = nullptr;
    std::string sid;
    if (item && Rd(item, off::RO_data, gd)) GameDataSid(gd, sid);
    return sid;
}

bool CallDrawWeapon(Character* c, const std::string& itemSid, const std::string& section) {
    void* inv = InventoryOf(c);
    if (!inv || itemSid.empty()) return false;
    for (void* it : InventoryItems(inv)) {
        if (ItemTemplate(it) != itemSid) continue;
        GameString s;
        if (!MakeGameString(section, s)) return false;
        return SehDraw(FnAddr(FnDrawWeapon), c, it, &s);   // the callee owns the by-value string
    }
    return false;
}

std::string DrawnFrom(const Character* c) {
    std::string s;
    if (Vtable(c) == Addr(rva::VtCharacterHuman)) ReadGameString(reinterpret_cast<const uint8_t*>(c) + 0x6E0, s);
    return s.empty() ? "-" : s;
}

bool CallSheathe(Character* c) { return IsCharacter(c) && CallVoid(FnAddr(FnSheatheWeapon), c); }

bool SetDestination(Character* c, const kc::Vec3& dest) {
    void* m = Movement(c);
    if (!m || !Finite(dest)) return false;
    void* fn = VSlot(m, slot::CM_setDestination);
    const float p[3] = {dest.x, dest.y, dest.z};
    return fn && CallSetDest(fn, m, p, HIGH_PRIORITY);
}

using VirtVec = void (*)(void* self, const float* v);
bool CallVec(void* fn, void* self, const float* v) {
    __try {
        reinterpret_cast<VirtVec>(fn)(self, v);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SetPositionSimple(Character* c, const kc::Vec3& pos) {
    void* m = Movement(c);
    if (!m || !Finite(pos)) return false;
    void* fn = VSlot(m, slot::CM_setPositionSimple);
    const float p[3] = {pos.x, pos.y, pos.z};
    return fn && CallVec(fn, m, p);
}

bool IsMoving(const Character* c) {
    void* m = Movement(c);
    bool moving = false;
    return m && Rd(m, off::CM_currentlyMoving, moving) && moving;
}

bool Halt(Character* c) {
    void* m = Movement(c);
    void* fn = m ? VSlot(m, slot::CM_halt) : nullptr;
    return fn && CallVoid(fn, m);
}

bool CallSetFrameSpeed(float speed) {
    GameWorld* w = World();
    return w && std::isfinite(speed) && CallFloat(FnAddr(FnSetFrameSpeedMultiplier), w, speed);
}

bool CallTogglePause(bool paused) {
    GameWorld* w = World();
    return w && CallBoolArg(FnAddr(FnTogglePause), w, paused);
}

bool CallUserPause(bool paused) {
    GameWorld* w = World();
    return w && CallBoolArg(FnAddr(FnUserPause), w, paused);
}

// ---------------------------------------------------------------- lot D: prisons
namespace {
constexpr uintptr_t CH_inSomething = 0x2F8, CH_inWhat = 0x300, CH_isChained = 0x320, CH_slaveOwner = 0x328, CH_stateBroadcast = 0x1A0;
constexpr uintptr_t CH_sentenceBegan = 0xF0 + 0x98, CH_sentence = 0xF0 + 0xA0;   // BountyManager crimes (+0xF0)
constexpr uintptr_t SBD_slaveState = 0x0, SBD_isSlaveOf = 0xE0, SBD_escaped = 0xE8, SBD_kidnapped = 0xE9;
constexpr int kUseInPrison = 2;
constexpr int kBuildingFunctionCage = 8;   // BF_CAGE
using FnPrisonSig = void (*)(void* c, bool on, void* cage);
using FnChainedSig = void (*)(void* c, bool on, const void* ownerHand);
using FnSlaveStateSig = void (*)(void* sbd, int state);
bool PrisonSeh(void* c, bool on, void* cage) {
    __try { reinterpret_cast<FnPrisonSig>(FnAddr(FnSetPrisonMode))(c, on, cage); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ChainedSeh(void* c, bool on, const void* hand) {
    __try { reinterpret_cast<FnChainedSig>(FnAddr(FnSetChainedMode))(c, on, hand); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SlaveStateSeh(void* sbd, int state) {
    __try { reinterpret_cast<FnSlaveStateSig>(FnAddr(FnSetSlaveState))(sbd, state); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void* StateBroadcastOf(Character* c) {
    void* sbd = nullptr;
    return IsCharacter(c) && Rd(c, CH_stateBroadcast, sbd) ? sbd : nullptr;
}
} // namespace

bool ReadCaptivity(Character* c, Captivity& out) {
    out = Captivity{};
    if (!IsCharacter(c) || !Rd(c, CH_inSomething, out.inSomething)) return false;
    if (out.inSomething == kUseInPrison) {
        kc::Handle cage;
        if (ReadHandle(reinterpret_cast<uint8_t*>(c) + CH_inWhat, cage) && cage.valid()) out.cage = ResolveObject(cage);
    }
    uint8_t chained = 0;
    Rd(c, CH_isChained, chained);
    out.chained = chained != 0;
    kc::Handle owner;
    if (ReadHandle(reinterpret_cast<uint8_t*>(c) + CH_slaveOwner, owner) && owner.valid()) out.slaveOwner = owner;
    if (void* sbd = StateBroadcastOf(c)) {
        uint8_t esc = 0, kid = 0;
        Rd(sbd, SBD_slaveState, out.slaveState);
        Rd(sbd, SBD_isSlaveOf, out.slaveOf);
        Rd(sbd, SBD_escaped, esc);
        Rd(sbd, SBD_kidnapped, kid);
        out.escaped = esc != 0;
        out.kidnapped = kid != 0;
    }
    Rd(c, CH_sentenceBegan, out.sentenceBegan);
    Rd(c, CH_sentence, out.sentence);
    if (!std::isfinite(out.sentence) || out.sentence < 0 || out.sentence > 1e6f) out.sentence = 0;
    if (out.slaveState < 0 || out.slaveState > 3) out.slaveState = 0;
    return true;
}

bool WriteCaptivity(Character* c, const Captivity& s) {
    if (!IsCharacter(c)) return false;
    const uint8_t chained = s.chained ? 1 : 0;
    Wr(c, CH_isChained, chained);
    alignas(8) uint8_t hand[off::HandSize];
    kc::Handle owner = s.slaveOwner;
    if (!owner.valid()) owner = kc::Handle{0xB, 0, 0, 0, 0};   // what the game keeps for "nobody"
    MakeHand(owner, hand);
    // only the identity fields: the hand's own vtable stays
    SafeCopy(reinterpret_cast<uint8_t*>(c) + CH_slaveOwner + 8, hand + 8, off::HandSize - 8);
    if (void* sbd = StateBroadcastOf(c)) {
        const uint8_t esc = s.escaped ? 1 : 0, kid = s.kidnapped ? 1 : 0;
        Wr(sbd, SBD_slaveState, s.slaveState);
        Wr(sbd, SBD_isSlaveOf, s.slaveOf);
        Wr(sbd, SBD_escaped, esc);
        Wr(sbd, SBD_kidnapped, kid);
    }
    Wr(c, CH_sentenceBegan, s.sentenceBegan);
    Wr(c, CH_sentence, s.sentence);
    return true;
}

bool SetPrisonMode(Character* c, bool on, void* cage) {
    if (!IsCharacter(c) || (on && !cage)) return false;
    return PrisonSeh(c, on, cage);
}

bool CallSetChainedMode(Character* c, bool on) {
    if (!IsCharacter(c)) return false;
    alignas(8) uint8_t hand[off::HandSize];
    MakeHand(kc::Handle{0xB, 0, 0, 0, 0}, hand);   // no owner
    return ChainedSeh(c, on, hand);
}

bool CallSetSlaveState(Character* c, int state) {
    void* sbd = StateBroadcastOf(c);
    return sbd && state >= 0 && state <= 3 && SlaveStateSeh(sbd, state);
}

std::string FactionSidOf(void* faction) {
    void* gd = nullptr;
    std::string sid;
    if (faction && Rd(faction, off::FAC_data, gd) && gd) GameDataSid(gd, sid);
    return sid;
}

void* FactionBySid(const std::string& sid) { return sid.empty() ? nullptr : FindFaction(sid); }

void* NearestCage(const kc::Vec3& at, float radius) {
    std::vector<void*> around;
    ObjectsNear(at, radius, around);
    void* best = nullptr;
    float bestD = radius * radius;
    for (void* o : around) {
        kc::Vec3 p;
        if (BuildingFunctionOf(o) != kBuildingFunctionCage || !ObjectPosition(o, p)) continue;
        const float d = (p.x - at.x) * (p.x - at.x) + (p.y - at.y) * (p.y - at.y) + (p.z - at.z) * (p.z - at.z);
        if (d < bestD) { bestD = d; best = o; }
    }
    return best;
}

// ---- lot E: buildings
bool ReadRaw(const void* obj, uintptr_t offset, void* out, size_t n) {
    return obj && SafeCopy(out, reinterpret_cast<const uint8_t*>(obj) + offset, n);
}

bool WriteRaw(void* obj, uintptr_t offset, const void* in, size_t n) {
    return obj && SafeCopy(reinterpret_cast<uint8_t*>(obj) + offset, in, n);
}

void* GameDataBySid(const std::string& sid) { return FindGameData(sid); }

bool GameDataSidOf(const void* gd, std::string& out) { return GameDataSid(gd, out); }

void ItemTemplates(const std::string& part, std::vector<std::pair<std::string, std::string>>& out, size_t max) {
    out.clear();
    if (!g_gameDataBuilt) BuildGameDataIndex();
    // Many records share a name with an item (a research, a dialogue line... "Building Materials"
    // is also type 49, which the item factory refuses): weapons, armour and items (itemType 2, 3, 4)
    // first, exact names first among them.
    std::vector<std::tuple<int, std::string, std::string>> all;
    for (const auto& [sid, gd] : g_gameDataBySid) {
        int type = -1;
        std::string name;
        if (!Rd(gd, off::GD_type, type) || type == 0 || !TemplateDisplayName(sid, name)) continue;   // not a BUILDING
        if (!part.empty() && name.find(part) == std::string::npos && sid != part) continue;
        const int rank = (type >= 2 && type <= 4 ? 0 : 2) + (name == part || sid == part ? 0 : 1);
        all.emplace_back(rank, sid, name);
    }
    std::stable_sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return std::get<0>(a) < std::get<0>(b); });
    for (auto& [rank, sid, name] : all) {
        if (out.size() >= max) break;
        out.emplace_back(std::move(sid), std::move(name));
    }
}

void GameDataOfType(int type, std::vector<void*>& out, size_t max) {
    out.clear();
    if (!g_gameDataBuilt) BuildGameDataIndex();
    std::vector<std::pair<std::string, void*>> all;
    for (const auto& [sid, gd] : g_gameDataBySid) {
        int t = -1;
        if (Rd(gd, off::GD_type, t) && t == type) all.emplace_back(sid, gd);
    }
    std::sort(all.begin(), all.end());   // the same order on every machine
    for (auto& [sid, gd] : all) {
        if (out.size() >= max) break;
        out.push_back(gd);
    }
}

void BuildingTemplates(const std::string& part, std::vector<std::pair<std::string, std::string>>& out, size_t max) {
    out.clear();
    if (!g_gameDataBuilt) BuildGameDataIndex();
    for (const auto& [sid, gd] : g_gameDataBySid) {
        int type = -1;
        std::string name;
        if (!Rd(gd, off::GD_type, type) || type != 0 || !TemplateDisplayName(sid, name)) continue;   // itemType BUILDING
        if (!part.empty() && name.find(part) == std::string::npos && sid.find(part) == std::string::npos) continue;
        out.emplace_back(sid, name);
        if (out.size() >= max) break;
    }
}

bool DestroyAnyObject(void* obj) {
    GameWorld* w = World();
    return w && obj && CallDestroy(FnAddr(FnWorldDestroy), w, obj);
}

// ---- admin console
namespace {
std::mutex g_godMutex;
std::unordered_set<const void*> g_god;
} // namespace

bool HealCompletely(Character* c) {
    return IsCharacter(c) && CallVoid(FnAddr(FnHealCompletely), c);
}

void SetGodMode(Character* c, bool on) {
    std::lock_guard<std::mutex> lk(g_godMutex);
    if (on) g_god.insert(c);
    else g_god.erase(c);
}

void ForgetGodModes() {
    std::lock_guard<std::mutex> lk(g_godMutex);
    g_god.clear();
}

bool GodMode(const void* c) {
    std::lock_guard<std::mutex> lk(g_godMutex);
    return g_god.count(c) != 0;
}

void SetGodModes(const std::vector<Character*>& chars) {
    std::lock_guard<std::mutex> lk(g_godMutex);
    g_god.clear();
    for (Character* c : chars) if (c) g_god.insert(c);
}

size_t GodModeCount() {
    std::lock_guard<std::mutex> lk(g_godMutex);
    return g_god.size();
}

// ---- tools in hands (mining...)
namespace {
constexpr uintptr_t CH_body = 0x648;          // CharBody*
constexpr uintptr_t CB_currentAction = 0x68;  // Tasker* being run
constexpr uintptr_t TK_tool = 0x88;           // Task_OperateMachine: the tool it put in the hands
constexpr uintptr_t kVtOperateMachine = 0x16BE898;
using FnHandSig = void (*)(void* body, const void* name, void* item);
bool HandSeh(void* fn, void* obj, const void* name, void* item) {
    __try { reinterpret_cast<FnHandSig>(fn)(obj, name, item); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// what the job calls on: CharBody->vt[0x48]() (the character's attachments owner)
void* HandOwner(Character* c) {
    void* body = nullptr;
    if (!IsCharacter(c) || !Rd(c, CH_body, body) || !body) return nullptr;
    void* fn = VSlot(body, 0x48);
    return fn ? CallNoArgPtrOn(fn, body) : nullptr;
}
} // namespace

void* JobTool(Character* c) {
    void* body = nullptr;
    void* task = nullptr;
    uintptr_t vt = 0;
    void* tool = nullptr;
    if (!IsCharacter(c) || !Rd(c, CH_body, body) || !body || !Rd(body, CB_currentAction, task) || !task || !Rd(task, 0, vt)) return nullptr;
    if (vt != Addr(kVtOperateMachine)) return nullptr;
    return Rd(task, TK_tool, tool) ? tool : nullptr;
}

void* SetHandTool(Character* c, void* current, const std::string& sid) {
    void* owner = HandOwner(c);
    if (!owner) return current;
    alignas(8) uint8_t name[kGameStringSize];
    GameStringView("hands", name);
    if (current) {
        if (void* fn = VSlot(owner, 0x1A0)) HandSeh(fn, owner, name, current);   // out of the hands
        DestroyItem(current);
        current = nullptr;
    }
    if (sid.empty()) return nullptr;
    kc::ItemState s;
    s.templateSid = sid;
    void* item = CreateItemFromState(s);
    if (!item) return nullptr;
    if (void* fn = VSlot(owner, 0x198); fn && HandSeh(fn, owner, name, item)) return item;   // into the hands
    DestroyItem(item);
    return nullptr;
}

// ---- fix G6: floors
// CharMovement::floorGroup (+0x334): getCurrentFloor() derives the floor from it, and
// _setPositionAndTeleport(p, floor) sets it to floor + 9. Taking the stairs changes it; a character
// only placed (as on a client) keeps its old one.
namespace {
constexpr uintptr_t CM_floorGroup = 0x334;
}

bool ReadFloorGroup(const Character* c, int32_t& group) {
    void* m = Movement(c);
    return m && Rd(m, CM_floorGroup, group);
}

bool WriteFloorGroup(Character* c, int32_t group) {
    void* m = Movement(c);
    int32_t cur = 0;
    if (!m || !Rd(m, CM_floorGroup, cur)) return false;
    if (cur == group) return true;
    __try {
        *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(m) + CM_floorGroup) = group;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// _setPositionAndTeleport(p, floor) (0x65E940, checked at start): halts, sets the position, then
// floorGroup = floor + 9 when floor >= 0 (the 0xC0 teleport calls it with -1: floor untouched).
// Groups below 9 have no floor number and cannot be set this way.
namespace {
using FnSetPosFloor = void (*)(void* self, const float* pos, int floor);
bool CallSetPosFloor(void* fn, void* self, const float* pos, int floor) {
    __try {
        reinterpret_cast<FnSetPosFloor>(fn)(self, pos, floor);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
} // namespace

bool PlaceOnFloor(Character* c, int32_t group) {
    void* m = Movement(c);
    kc::Vec3 p;
    if (!m || group < 9 || !GetPosition(c, p) || !Finite(p)) return false;
    const float v[3] = {p.x, p.y, p.z};
    return CallSetPosFloor(FnAddr(FnCMSetPositionAndTeleport), m, v, group - 9);
}

// ---- fix G5: selection kept, job lists
namespace {
using FnIntOfConst = int (*)(const void* self);
using FnIntOfInt = int (*)(const void* self, int i);
using FnTwoInts = void (*)(void* self, int a, int b);
bool IntOfSeh(void* fn, const void* self, int& out) {
    __try { out = reinterpret_cast<FnIntOfConst>(fn)(self); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool IntOfIntSeh(void* fn, const void* self, int i, int& out) {
    __try { out = reinterpret_cast<FnIntOfInt>(fn)(self, i); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool TwoIntsSeh(void* fn, void* self, int a, int b) {
    __try { reinterpret_cast<FnTwoInts>(fn)(self, a, b); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// the job functions go through Character::ai (+0x650) -> AI::orders (+0x20)
bool HasOrdersReceiver(Character* c) {
    void* ai = nullptr;
    void* orders = nullptr;
    return IsCharacter(c) && Rd(c, 0x650, ai) && ai && Rd(ai, 0x20, orders) && orders;
}
} // namespace

void KeepSelection(const std::function<void()>& fn) {
    PlayerInterface* pi = Player();
    if (!pi) { fn(); return; }
    SavedSelection saved;
    SaveSelection(pi, saved);
    fn();
    std::vector<kc::Handle> after;
    SelectedHandles(after);
    void* nowPlatoon = nullptr;
    Rd(pi, PI_currentPlatoon, nowPlatoon);
    if (SameSelection(after, saved.sel) && nowPlatoon == saved.platoon) return;
    if (!RestoreSelection(pi, saved)) ++g_selectionRestoreFailures;
}

void UnselectObject(void* obj) {
    PlayerInterface* pi = Player();
    if (!pi || !obj) return;
    using FnSel = void (*)(void*, void*, bool);
    reinterpret_cast<FnSel>(FnAddr(FnObjectSelected))(pi, obj, false);
}

int PermajobCount(Character* c) {
    int n = 0;
    if (!HasOrdersReceiver(c) || !IntOfSeh(FnAddr(FnCharPermajobCount), c, n) || n < 0 || n > 256) return 0;
    return n;
}

int PermajobType(Character* c, int slot) {
    int t = -1;
    if (slot < 0 || slot >= PermajobCount(c) || !IntOfIntSeh(FnAddr(FnCharGetPermajob), c, slot, t)) return -1;
    return t;
}

bool RemovePermajob(Character* c, int slot) {
    return slot >= 0 && slot < PermajobCount(c) && CallInt(FnAddr(FnCharRemovePermajob), c, slot);
}

bool MovePermajob(Character* c, int from, int to) {
    const int n = PermajobCount(c);
    return from >= 0 && from < n && to >= 0 && to < n && from != to && TwoIntsSeh(FnAddr(FnCharMovePermajob), c, from, to);
}

bool RemoveJobKind(Character* c, int task) {
    return HasOrdersReceiver(c) && task >= 0 && CallInt(FnAddr(FnCharRemoveJob), c, task);
}

// ---- squad window: the player faction's squads, their order, the squad window's own actions
namespace {
constexpr uintptr_t FA_platoonCount = 0x210, FA_platoonData = 0x218;   // Faction::activePlatoons (lektor<Platoon*>)
constexpr uintptr_t PL_activePlatoon = 0x1D8, AP_me = 0x78, AP_count = 0x58;   // Platoon -> ActivePlatoon -> Platoon; members
constexpr uintptr_t PI_deadSquad = 0x2C8;                               // PlayerInterface::deadPlayerSquad (hand)
using FnPtrInt = void (*)(void*, void*, int);
using FnPtrPtr = void (*)(void*, void*);
using FnPtrOfInt = const void* (*)(const void*, int);
using FnAddJobC = void (*)(void*, int, void*, bool, bool, const float*);
bool PtrIntSeh(void* fn, void* a, void* b, int i) {
    __try { reinterpret_cast<FnPtrInt>(fn)(a, b, i); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool PtrPtrSeh(void* fn, void* a, void* b) {
    __try { reinterpret_cast<FnPtrPtr>(fn)(a, b); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool PtrOfIntSeh(void* fn, const void* self, int i, const void*& out) {
    __try { out = reinterpret_cast<FnPtrOfInt>(fn)(self, i); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool AddJobSeh(void* fn, void* c, int task, void* subject, const float* loc) {
    __try { reinterpret_cast<FnAddJobC>(fn)(c, task, subject, true, true, loc); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void* PlayerFaction() {
    PlayerInterface* pi = Player();
    void* f = nullptr;
    return pi && Rd(pi, PI_faction, f) ? f : nullptr;
}
void* PlatoonOf(void* squad) {
    void* p = nullptr;
    return squad && Rd(squad, AP_me, p) ? p : nullptr;
}
} // namespace

namespace {
// The game's own test for the dead squad (MainBarGUI 0x416140 / 0x4167B0, GameWorld's GUI events
// 0x7B65B0): the Platoon's hand (+0x58) == PlayerInterface::deadPlayerSquad (+0x2C8), with
// hand::operator== (0xCD060: type, container, container serial, index, serial). Never through
// kc::Handle::valid(): the player's platoons have hands with index and serial 0, so the old test
// (ObjectHandle(...) && h == dead) never matched and the dead squad was treated as a squad like
// the others (the 20:20 soak: the host sent it, the client made a regular "__DEAD_SQUAD__").
bool DeadSquadPlatoon(const void* platoon) {
    PlayerInterface* pi = Player();
    uint8_t a[off::HandSize], b[off::HandSize];
    if (!pi || !platoon || !SafeCopy(a, static_cast<const uint8_t*>(platoon) + off::RO_handle, sizeof(a)) ||
        !SafeCopy(b, reinterpret_cast<const uint8_t*>(pi) + PI_deadSquad, sizeof(b)))
        return false;
    uint32_t ta = 0, tb = 0;
    std::memcpy(&ta, a + off::H_type, 4);
    std::memcpy(&tb, b + off::H_type, 4);
    if (tb == 0xB) return false;   // no dead squad yet (a null hand names nothing)
    if (ta == tb && std::memcmp(a + off::H_container, b + off::H_container, off::H_serial + 4 - off::H_container) == 0) return true;
    // the game's own name for it, in case the hand ever reads differently
    std::string name;
    return ReadGameString(static_cast<const uint8_t*>(platoon) + off::RO_name, name) && kc::IsDeadSquadName(name);
}
} // namespace

bool IsDeadSquad(void* squad) {
    return squad && DeadSquadPlatoon(PlatoonOf(squad));
}

bool InPlayerSquad(Character* c) {
    void* sq = SquadOf(c);
    void* platoon = PlatoonOf(sq);
    void* f = PlayerFaction();
    uint32_t n = 0;
    void** data = nullptr;
    if (!platoon || !f || !Rd(f, FA_platoonCount, n) || !Rd(f, FA_platoonData, data) || !data || n > 4096) return false;
    for (uint32_t i = 0; i < n; ++i) {
        void* p = nullptr;
        if (Rd(data, i * sizeof(void*), p) && p == platoon) return true;
    }
    return false;
}

void PlayerSquads(std::vector<void*>& out) {
    out.clear();
    void* f = PlayerFaction();
    uint32_t n = 0;
    void** data = nullptr;
    if (!f || !Rd(f, FA_platoonCount, n) || !Rd(f, FA_platoonData, data) || !data || n > 4096) return;
    for (uint32_t i = 0; i < n; ++i) {
        void* platoon = nullptr;
        void* active = nullptr;
        if (!Rd(data, i * sizeof(void*), platoon) || !platoon || !Rd(platoon, PL_activePlatoon, active) || !active) continue;
        if (DeadSquadPlatoon(platoon)) continue;
        out.push_back(active);
    }
}

// A squad's id: its Platoon's address, unique and stable while the squad lives (a synthetic handle,
// type kSquadIdType). Not Platoon::handle (+0x58): in game, the squad window's squads gave no valid
// one, so the host sent no squad at all (squadui and suite, 10 Oct.).
bool SquadHandle(void* squad, kc::Handle& out) {
    void* platoon = PlatoonOf(squad);
    out = kc::Handle{};
    if (!platoon) return false;
    const uint64_t p = reinterpret_cast<uint64_t>(platoon);
    out.type = kSquadIdType;
    out.index = uint32_t(p);
    out.serial = uint32_t(p >> 32) | 0x80000000u;   // never 0: valid()
    return true;
}

bool SwapInSquad(void* squad, int a, int b) {
    const int n = SquadSize(squad);
    return a != b && a >= 0 && b >= 0 && a < n && b < n && TwoIntsSeh(FnAddr(FnSquadSwapCharacters), squad, a, b);
}

int SquadFactionIndex(void* squad) {
    void* f = PlayerFaction();
    void* platoon = PlatoonOf(squad);
    uint32_t n = 0;
    void** data = nullptr;
    if (!f || !platoon || !Rd(f, FA_platoonCount, n) || !Rd(f, FA_platoonData, data) || !data || n > 4096) return -1;
    for (uint32_t i = 0; i < n; ++i) {
        void* p = nullptr;
        if (Rd(data, i * sizeof(void*), p) && p == platoon) return int(i);
    }
    return -1;
}

bool SetSquadOrder(void* squad, int factionIndex) {
    void* f = PlayerFaction();
    void* platoon = PlatoonOf(squad);
    return f && platoon && factionIndex >= 0 && !IsDeadSquad(squad) && PtrIntSeh(FnAddr(FnChangePlatoonIndex), f, platoon, factionIndex);
}

bool DestroySquad(void* squad) {
    void* f = PlayerFaction();
    void* platoon = PlatoonOf(squad);
    return f && platoon && SquadSize(squad) == 0 && !IsDeadSquad(squad) && PtrPtrSeh(FnAddr(FnDestroyPlatoon), f, platoon);
}

int SquadSize(void* squad) {
    int n = 0;
    return squad && Rd(squad, AP_count, n) && n >= 0 && n < 4096 ? n : 0;
}

bool SetCharacterName(Character* c, const std::string& name) {
    std::string cur;
    if (!IsCharacter(c) || name.empty()) return false;
    if (CharacterName(c, cur) && cur == name) return true;
    void* fn = VSlot(c, slot::RO_setName);
    alignas(8) uint8_t gs[0x28];
    GameStringView(name, gs);
    return fn && CallStrBool(fn, c, gs, false);
}

bool PermajobTarget(Character* c, int slot, kc::Handle& subject, kc::Vec3& location) {
    subject = kc::Handle{};
    location = {};
    if (slot < 0 || slot >= PermajobCount(c)) return false;
    const void* t = nullptr;
    if (!PtrOfIntSeh(FnAddr(FnCharGetPermajobData), c, slot, t) || !t) return false;
    ReadHandle(static_cast<const uint8_t*>(t) + 0x10, subject);   // Tasker::subject
    return RdVec(t, 0x58, location);                                // Tasker::location
}

bool AddPermajob(Character* c, int task, void* subject, const kc::Vec3& location) {
    if (!HasOrdersReceiver(c) || task < 0) return false;
    const float loc[3] = {location.x, location.y, location.z};
    return AddJobSeh(FnAddr(FnCharAddJob), c, task, subject, loc);
}

// ---- squad bar integrity (MainBarGUI, the portraits at the bottom of the screen) [D]
// MainBarGUI (*0x21337C0): +0x210 its tabs (0x40 bytes each: +0x0 PortraitMainItemBox*, +0x8 the
// ActivePlatoon), +0x218 their count. PortraitMainItemBox +0xF0: MyGUI::ItemBox, whose items are
// MyGUI::Any (+0x6A8 / +0x6B0: begin / end, 8 bytes each: the Holder*). Holder<PortraitData*>: vtable
// 0x16D1FB0, +0x8 the PortraitData*. Every PortraitData lives in the PortraitManager (*0x212EBE8,
// built once: guard bit 0 of 0x212EBF4), std::map<hand, ...> at +0x70 (head +0x78; node: +0x0 left,
// +0x8 parent, +0x10 right, +0x38 PortraitData*, +0x49 isnil), and is only deleted with the whole
// world (0x414850, from the world reset 0x36CB80). Only the shown squad's tab is refilled when a
// squad changes (0x4167B0 -> 0x415880); a rebuild of the tabs (0x416140, after a squad empties,
// fills or is removed) resizes every tab kept, which redraws all its items: a PortraitData* that is
// not the manager's is read there (kenshi_x64+0x412D96, the 14:54 and 20:20 soak crashes).
namespace {
constexpr uintptr_t kMainBarGui = 0x21337C0, kPortraitMgr = 0x212EBE8, kPortraitMgrGuard = 0x212EBF4, kVtPortraitHolder = 0x16D1FB0;
constexpr uintptr_t MB_tabs = 0x210, MB_tabCount = 0x218, kTabSize = 0x40, TAB_box = 0x0, TAB_squad = 0x8;
constexpr uintptr_t PB_itemBox = 0xF0, IB_itemsBegin = 0x6A8, IB_itemsEnd = 0x6B0;
constexpr uintptr_t PM_head = 0x78, TN_left = 0x0, TN_parent = 0x8, TN_right = 0x10, TN_portrait = 0x38, TN_isnil = 0x49;
using FnRemoveAll = void (*)(void*);
using FnBarUpdate = void (*)(void*);
bool RemoveAllSeh(void* fn, void* box) {
    __try { reinterpret_cast<FnRemoveAll>(fn)(box); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool BarUpdateSeh(void* fn, void* bar) {
    __try { reinterpret_cast<FnBarUpdate>(fn)(bar); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool IsNil(void* node) {
    uint8_t nil = 1;
    return !node || !Rd(node, TN_isnil, nil) || nil != 0;
}
// Every PortraitData the game has (in-order walk of the manager's map, as 0x414850 does).
bool GamePortraits(std::unordered_set<uintptr_t>& out) {
    out.clear();
    uint32_t guard = 0;
    void* pm = nullptr;
    void* head = nullptr;
    void* n = nullptr;
    if (!Rd(reinterpret_cast<void*>(Addr(kPortraitMgrGuard)), 0, guard) || !(guard & 1) || !Rd(reinterpret_cast<void*>(Addr(kPortraitMgr)), 0, pm) || !pm ||
        !Rd(pm, PM_head, head) || !head || !Rd(head, TN_left, n))
        return false;
    for (int steps = 0; n && n != head; ++steps) {
        if (steps > 8192) return false;
        void* pd = nullptr;
        if (!Rd(n, TN_portrait, pd)) return false;
        out.insert(reinterpret_cast<uintptr_t>(pd));
        void* r = nullptr;
        if (!Rd(n, TN_right, r)) return false;
        if (!IsNil(r)) {
            n = r;
            for (void* l = nullptr; Rd(n, TN_left, l) && !IsNil(l);) n = l;
        } else {
            void* p = nullptr;
            if (!Rd(n, TN_parent, p)) return false;
            for (void* pr = nullptr; !IsNil(p) && Rd(p, TN_right, pr) && pr == n;) {
                n = p;
                if (!Rd(n, TN_parent, p)) return false;
            }
            n = p;
        }
    }
    return true;
}
std::unordered_set<uintptr_t> g_portraitCache;   // game thread only (the squad bar's draw, the tick)
} // namespace

bool IsGamePortrait(const void* portraitData) {
    const auto p = reinterpret_cast<uintptr_t>(portraitData);
    if (!p) return false;
    if (g_portraitCache.count(p)) return true;
    // a portrait made since (a new member): the manager is read again; unreadable (another build of
    // the game?): the game draws as it would without the mod
    if (!GamePortraits(g_portraitCache)) { g_portraitCache.clear(); return true; }
    return g_portraitCache.count(p) != 0;
}

bool CheckSquadBar(SquadBarReport& out, bool repair) {
    out = SquadBarReport{};
    void* bar = nullptr;
    void* tabs = nullptr;
    uint64_t count = 0;
    if (!Rd(reinterpret_cast<void*>(Addr(kMainBarGui)), 0, bar) || !bar || !Rd(bar, MB_tabs, tabs) || !Rd(bar, MB_tabCount, count) || count > 256) return false;
    if (count && !tabs) return false;
    // read again every frame: the cache of the draw hook (IsGamePortrait) never outlives a world
    if (!GamePortraits(g_portraitCache)) { g_portraitCache.clear(); return false; }
    const std::unordered_set<uintptr_t>& portraits = g_portraitCache;
    std::vector<void*> squads;
    PlayerSquads(squads);
    static void* removeAll = nullptr;
    if (!removeAll)
        if (HMODULE gui = GetModuleHandleW(L"MyGUIEngine_x64.dll")) removeAll = reinterpret_cast<void*>(GetProcAddress(gui, "?removeAllItems@ItemBox@MyGUI@@QEAAXXZ"));
    const uintptr_t holderVt = Addr(kVtPortraitHolder);
    bool refill = false;
    for (uint64_t t = 0; t < count; ++t) {
        const uint8_t* tab = static_cast<const uint8_t*>(tabs) + t * kTabSize;
        void* pbox = nullptr;
        void* squad = nullptr;
        void* box = nullptr;
        void** begin = nullptr;
        void** end = nullptr;
        if (!Rd(tab, TAB_box, pbox) || !Rd(tab, TAB_squad, squad)) return false;
        ++out.tabs;
        const bool known = std::find(squads.begin(), squads.end(), squad) != squads.end();
        if (!known) ++out.staleTabs;   // a tab whose squad is not the player's (any more): the next rebuild drops it
        if (!pbox || !Rd(pbox, PB_itemBox, box) || !box || !Rd(box, IB_itemsBegin, begin) || !Rd(box, IB_itemsEnd, end)) continue;
        const ptrdiff_t n = end - begin;
        if (n < 0 || n > 256) { ++out.badTabs; continue; }
        int bad = 0;
        bool holdersOk = true;
        for (ptrdiff_t i = 0; i < n; ++i) {
            ++out.items;
            void* holder = nullptr;
            uintptr_t vt = 0;
            void* pd = nullptr;
            if (!Rd(begin, size_t(i) * sizeof(void*), holder) || !holder || !Rd(holder, 0, vt) || vt != holderVt) { holdersOk = false; ++bad; continue; }
            if (!Rd(holder, 8, pd) || !portraits.count(reinterpret_cast<uintptr_t>(pd))) {
                ++bad;
                if (out.firstBad.empty()) {
                    char b[160];
                    std::string name;
                    if (known) SquadName(squad, name);
                    snprintf(b, sizeof(b), "tab %llu ('%s'%s) item %lld holds %p", static_cast<unsigned long long>(t), name.c_str(), known ? "" : ", not a player squad",
                             static_cast<long long>(i), pd);
                    out.firstBad = b;
                }
            }
        }
        if (!bad) continue;
        out.badItems += bad;
        ++out.badTabs;
        // the items go (MyGUI deletes each Holder by its vtable: only when every one is intact); the
        // shown squad's tab is refilled by the game below, another one when it is shown next
        if (repair && holdersOk && removeAll && RemoveAllSeh(removeAll, box)) { ++out.repairedTabs; refill = true; }
    }
    if (refill) BarUpdateSeh(FnAddr(FnMainBarUpdateCurrent), bar);
    return true;
}

} // namespace kenshi
