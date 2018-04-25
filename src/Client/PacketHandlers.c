#include "Deflate.h"
#include "Utils.h"
#include "ServerConnection.h"
#include "Stream.h"
#include "Game.h"
#include "Entity.h"
#include "Platform.h"
#include "Screens.h"
#include "World.h"
#include "Event.h"
#include "ExtMath.h"
#include "SelectionBox.h"
#include "Chat.h"
#include "Inventory.h"
#include "Block.h"
#include "IModel.h"
#include "Funcs.h"
#include "Lighting.h"
#include "AsyncDownloader.h"
#include "Drawer2D.h"
#include "ErrorHandler.h"

/*########################################################################################################################*
*-----------------------------------------------------Common handlers-----------------------------------------------------*
*#########################################################################################################################*/
void Set(UInt8 opcode, void(*callback)(Stream* stream), UInt16 size) {
}

String Handlers_ReadString(Stream* stream, STRING_REF UInt8* strBuffer) {
	UInt8 buffer[STRING_SIZE];
	Stream_Read(stream, buffer, sizeof(buffer));
	Int32 i, length = 0;

	for (i = STRING_SIZE - 1; i >= 0; i--) {
		UInt8 code = buffer[i];
		if (code == NULL || code == ' ') continue;
		length = i + 1; break;
	}

	String str = String_InitAndClear(strBuffer, STRING_SIZE);
	for (i = 0; i < length; i++) { String_Append(&str, buffer[i]); }
	return str;
}

void Handlers_WriteString(Stream* stream, STRING_PURE String* value) {
	UInt8 buffer[STRING_SIZE];

	Int32 i, count = min(value->length, STRING_SIZE);
	for (i = 0; i < count; i++) {
		UInt8 c = value->buffer[i];
		if (c == '&') c = '%'; /* escape colour codes */
		buffer[i] = c;
	}

	for (; i < STRING_SIZE; i++) { buffer[i] = ' '; }
	Stream_Write(stream, buffer, STRING_SIZE);
}

#define Handlers_ReadBlock(stream) Stream_ReadU8(stream)
#define Handlers_WriteBlock(stream, value) Stream_WriteU8(stream, value)


/*########################################################################################################################*
*----------------------------------------------------Classic protocol-----------------------------------------------------*
*#########################################################################################################################*/
bool receivedFirstPosition;
DateTime mapReceiveStart;
InflateState compState;
Stream compStream;
GZipHeader gzipHeader;
Int32 mapSizeIndex, mapIndex, mapVolume;
UInt8 mapSize[4];
UInt8* map;
FixedBufferStream mapPartStream;
Screen* prevScreen;
bool prevCursorVisible;

void ClassicProtocol_Init(void) {
	mapPartStream = new FixedBufferStream(net.reader.buffer);
	ClassicProtocol_Reset();
}

void ClassicProtocol_Reset(void) {
	Set(OPCODE_HANDSHAKE, HandleHandshake, 131);
	Set(OPCODE_PING, HandlePing, 1);
	Set(OPCODE_LEVEL_INIT, HandleLevelInit, 1);
	Set(OPCODE_LEVEL_DATA_CHUNK, HandleLevelDataChunk, 1028);
	Set(OPCODE_LEVEL_FINALISE, HandleLevelFinalise, 7);
	Set(OPCODE_SET_BLOCK, HandleSetBlock, 8);

	Set(OPCODE_ADD_ENTITY, HandleAddEntity, 74);
	Set(OPCODE_ENTITY_TELEPORT, HandleEntityTeleport, 10);
	Set(OPCODE_RELPOS_AND_ORIENTATION_UPDATE, HandleRelPosAndOrientationUpdate, 7);
	Set(OPCODE_RELPOS_UPDATE, HandleRelPositionUpdate, 5);
	Set(OPCODE_ORIENTATION_UPDATE, HandleOrientationUpdate, 4);
	Set(OPCODE_REMOVE_ENTITY, HandleRemoveEntity, 2);

	Set(OPCODE_MESSAGE, HandleMessage, 66);
	Set(OPCODE_KICK, HandleKick, 65);
	Set(OPCODE_SET_PERMISSION, HandleSetPermission, 2);
}


void HandleHandshake(Stream* stream) {
	UInt8 protocolVer = Stream_ReadU8(stream);
	ReadString(stream, &ServerConnection_ServerName);
	ReadString(stream, &ServerConnection_ServerMOTD);
	Chat_SetLogName(&ServerConnection_ServerName);

	HacksComp* hacks = &LocalPlayer_Instance.Hacks;
	HacksComp_SetUserType(hacks, Stream_ReadU8(stream), !cpe_blockPerms);
	
	String_Set(&hacks->HacksFlags, &ServerConnection_ServerName);
	String_AppendString(&hacks->HacksFlags, &ServerConnection_ServerMOTD);
	HacksComp_UpdateState(hacks);
}

void HandlePing(Stream* stream) { }

void HandleLevelInit(Stream* stream) {
	if (gzipStream == null) StartLoadingState();

	/* Fast map puts volume in header, doesn't bother with gzip */
	if (cpe_fastMap) {
		mapVolume = Stream_ReadI32_BE(stream);
		gzipHeader.Done = true;
		mapSizeIndex = 4;
		map = Platform_MemAlloc(mapVolume);
		if (map == NULL) ErrorHandler_Fail("Failed to allocate memory for map");
	}
}

void StartLoadingState() {
	World_Reset();
	Event_RaiseVoid(&WorldEvents_NewMap);

	prevScreen = Gui_Active;
	if (prevScreen is LoadingMapScreen) {
		prevScreen = NULL;
	}
	prevCursorVisible = Game_GetCursorVisible();

	Gui_SetNewScreen(new LoadingMapScreen(game, net.ServerName, net.ServerMotd), false);
	WoM_CheckMotd();
	receivedFirstPosition = false;
	GZipHeader_Init(&gzipHeader);
	gzipStream = new DeflateStream(mapPartStream, true);

	mapSizeIndex = 0;
	mapIndex = 0;
	Platform_CurrentUTCTime(&mapReceiveStart);
}

void HandleLevelDataChunk(Stream* stream) {
	// Workaround for some servers that send LevelDataChunk before LevelInit
	// due to their async packet sending behaviour.
	if (gzipStream == null) StartLoadingState();

	Int32 usedLength = Stream_ReadU16_BE(stream);
	mapPartStream.pos = 0;
	mapPartStream.bufferPos = reader.index;
	mapPartStream.len = usedLength;

	reader.Skip(1024);
	UInt8 value = Stream_ReadU8(stream); /* progress in original classic, but we ignore it */

	if (gzipHeader.Done || gzipHeader.ReadHeader(mapPartStream)) {
		if (mapSizeIndex < 4) {
			mapSizeIndex += gzipStream.Read(mapSize, mapSizeIndex, 4 - mapSizeIndex);
		}

		if (mapSizeIndex == 4) {
			if (map == NULL) {
				mapVolume = mapSize[0] << 24 | mapSize[1] << 16 | mapSize[2] << 8 | mapSize[3];
				map = Platform_MemAlloc(mapVolume);
				if (map == NULL) ErrorHandler_Fail("Failed to allocate memory for map");
			}
			mapIndex += gzipStream.Read(map, mapIndex, map.Length - mapIndex);
		}
	}

	Real32 progress = map == NULL ? 0.0f : (Real32)mapIndex / mapVolume;
	Event_RaiseReal32(&WorldEvents_MapLoading, progress);
}

void HandleLevelFinalise(Stream* stream) {
	Gui_SetNewScreen(NULL);
	Gui_Active = prevScreen;
	if (prevScreen != NULL && prevCursorVisible != Game_GetCursorVisible()) {
		Game_SetCursorVisible(prevCursorVisible);
	}
	prevScreen = NULL;

	Int32 mapWidth  = Stream_ReadU16_BE(stream);
	Int32 mapHeight = Stream_ReadU16_BE(stream);
	Int32 mapLength = Stream_ReadU16_BE(stream);

	DateTime now; Platform_CurrentUTCTime(&now);
	Int64 loadingMs = DateTime_MsBetween(&mapReceiveStart, &now);
	Utils.LogDebug("map loading took: " + loadingMs);

	World_SetNewMap(map, mapVolume, mapWidth, mapHeight, mapLength);
	Event_RaiseVoid(&WorldEvents_MapLoaded);
	Wom_CheckSendWomID();

	map = NULL;
	gzipStream.Dispose();
	gzipStream = null;
}

void HandleSetBlock(Stream* stream) {
	Int32 x = Stream_ReadU16_BE(stream);
	Int32 y = Stream_ReadU16_BE(stream);
	Int32 z = Stream_ReadU16_BE(stream);

	BlockID block = Handlers_ReadBlock(stream);
	if (World_IsValidPos(x, y, z)) {
		Game_UpdateBlock(x, y, z, block);
	}
}

void HandleAddEntity(Stream* stream) {
	EntityID id = Stream_ReadU8(stream);
	string name = reader.ReadString();
	string skin = name;
	net.CheckName(id, ref name, ref skin);
	net.AddEntity(id, name, skin, true);

	if (!net.addEntityHack) return;
	// Workaround for some servers that declare they support ExtPlayerList,
	// but doesn't send ExtAddPlayerName packets.
	net.AddTablistEntry(id, name, name, "Players", 0);
	net.needRemoveNames[id >> 3] |= (UInt8)(1 << (id & 0x7));
}

void HandleEntityTeleport(Stream* stream) {
	EntityID id = Stream_ReadU8(stream);
	ReadAbsoluteLocation(id, true);
}

void HandleRelPosAndOrientationUpdate(Stream* stream) {
	EntityID id = Stream_ReadU8(stream);
	Vector3 pos;
	pos.X = Stream_ReadI8(stream) / 32.0f;
	pos.Y = Stream_ReadI8(stream) / 32.0f;
	pos.Z = Stream_ReadI8(stream) / 32.0f;

	Real32 rotY  = Math_Packed2Deg(Stream_ReadU8(stream));
	Real32 headX = Math_Packed2Deg(Stream_ReadU8(stream));
	LocationUpdate update; LocationUpdate_MakePosAndOri(&update, pos, rotY, headX, true);
	net.UpdateLocation(id, update, true);
}

void HandleRelPositionUpdate(Stream* stream) {
	EntityID id = Stream_ReadU8(stream);
	Vector3 pos;
	pos.X = Stream_ReadI8(stream) / 32.0f;
	pos.Y = Stream_ReadI8(stream) / 32.0f;
	pos.Z = Stream_ReadI8(stream) / 32.0f;

	LocationUpdate update; LocationUpdate_MakePos(&update, pos, true);
	net.UpdateLocation(id, update, true);
}

void HandleOrientationUpdate(Stream* stream) {
	EntityID id = Stream_ReadU8(stream);
	Real32 rotY  = Math_Packed2Deg(Stream_ReadU8(stream));
	Real32 headX = Math_Packed2Deg(Stream_ReadU8(stream));

	LocationUpdate update; LocationUpdate_MakeOri(&update, rotY, headX);
	net.UpdateLocation(id, update, true);
}

void HandleRemoveEntity(Stream* stream) {
	EntityID id = Stream_ReadU8(stream);
	net.RemoveEntity(id);
}

void HandleMessage(Stream* stream) {
	UInt8 type = Stream_ReadU8(stream);
	/* Original vanilla server uses player ids in message types, 255 for server messages. */
	bool prepend = !cpe_useMessageTypes && type == 0xFF;

	if (!cpe_useMessageTypes) type = MSG_TYPE_NORMAL;
	string text = reader.ReadChatString(ref type);
	if (prepend) text = "&e" + text;

	if (!Utils.CaselessStarts(text, "^detail.user")) {
		Chat_AddOf(text, type);
	}
}

void HandleKick(Stream* stream) {
	UInt8 reasonBuffer[String_BufferSize(STRING_SIZE)];
	String reason = Handlers_ReadString(stream, reasonBuffer);
	String title = String_FromConst("&eLost connection to the server");
	Game_Disconnect(&title, &reason);
	net.Dispose();
}

void HandleSetPermission(Stream* stream) {
	HacksComp* hacks = &LocalPlayer_Instance.Hacks;
	HacksComp_SetUserType(hacks, Stream_ReadU8(stream), !cpe_blockPerms);
	HacksComp_UpdateHacksState(hacks);
}

void ReadAbsoluteLocation(Stream* stream, EntityID id, bool interpolate) {
	Int32 x, y, z;
	if (cpe_extEntityPos) {
		x = Stream_ReadI32_BE(stream); y = Stream_ReadI32_BE(stream); z = Stream_ReadI32_BE(stream);
	} else {
		x = Stream_ReadI16_BE(stream); y = Stream_ReadI16_BE(stream); z = Stream_ReadI16_BE(stream);
	}

	y -= 51; /* Convert to feet position */
	if (id == ENTITIES_SELF_ID) y += 22;

	Vector3 pos  = VECTOR3_CONST(x / 32.0f, y / 32.0f, z / 32.0f);
	Real32 rotY  = Math_Packed2Deg(Stream_ReadU8(stream));
	Real32 headX = Math_Packed2Deg(Stream_ReadU8(stream));

	if (id == ENTITIES_SELF_ID) receivedFirstPosition = true;
	LocationUpdate update; LocationUpdate_MakePosAndOri(&update, pos, rotY, headX, false);
	net.UpdateLocation(id, update, interpolate);
}

void WriteChat(Stream* stream, STRING_PURE String* text, bool partial) {
	Int32 payload = !net.SupportsPartialMessages ? ENTITIES_SELF_ID : (partial ? 1 : 0);
	Stream_WriteU8(stream, OPCODE_MESSAGE);
	Stream_WriteU8(stream, (UInt8)payload);
	Handlers_WriteString(stream, text);
}

void WritePosition(Stream* stream, Vector3 pos, Real32 rotY, Real32 headX) {
	Int32 payload = cpe_sendHeldBlock ? Inventory_SelectedBlock : ENTITIES_SELF_ID;
	Stream_WriteU8(stream, OPCODE_ENTITY_TELEPORT);
	Handlers_WriteBlock(stream, (BlockID)payload); /* held block when using HeldBlock, otherwise just 255 */

	if (cpe_extEntityPos) {
		Stream_WriteI32_BE(stream, (Int32)(pos.X * 32));
		Stream_WriteI32_BE(stream, (Int32)((Int32)(pos.Y * 32) + 51));
		Stream_WriteI32_BE(stream, (Int32)(pos.Z * 32));
	} else {
		Stream_WriteI16_BE(stream, (Int16)(pos.X * 32));
		Stream_WriteI16_BE(stream, (Int16)((Int32)(pos.Y * 32) + 51));
		Stream_WriteI16_BE(stream, (Int16)(pos.Z * 32));
	}
	Stream_WriteU8(stream, Math_Deg2Packed(rotY));
	Stream_WriteU8(stream, Math_Deg2Packed(headX));
}

void WriteSetBlock(Stream* stream, Int32 x, Int32 y, Int32 z, bool place, BlockID block) {
	Stream_WriteU8(stream, OPCODE_SET_BLOCK_CLIENT);
	Stream_WriteI16_BE(stream, x);
	Stream_WriteI16_BE(stream, y);
	Stream_WriteI16_BE(stream, z);
	Stream_WriteU8(stream, place ? 1 : 0);
	Handlers_WriteBlock(stream, block);
}

void WriteLogin(Stream* stream, STRING_PURE String* username, STRING_PURE String* verKey) {
	UInt8 payload = Game_UseCPE ? 0x42 : 0x00;
	Stream_WriteU8(stream, OPCODE_HANDSHAKE);

	Stream_WriteU8(stream, 7); /* protocol version */
	Handlers_WriteString(stream, username);
	Handlers_WriteString(stream, verKey);
	Stream_WriteU8(stream, payload);
}


/*########################################################################################################################*
*------------------------------------------------------CPE protocol-------------------------------------------------------*
*#########################################################################################################################*/

Int32 cpe_serverExtensionsCount;
bool cpe_sendHeldBlock, cpe_useMessageTypes;
Int32 cpe_envMapVer = 2, cpe_blockDefsExtVer = 2;
bool cpe_needD3Fix, cpe_extEntityPos, cpe_twoWayPing, cpe_blockPerms, cpe_fastMap;

void Reset(Game game) {
	cpe_serverExtensionsCount = 0;
	cpe_sendHeldBlock = false; cpe_useMessageTypes = false;
	cpe_envMapVer = 2; cpe_blockDefsExtVer = 2;
	cpe_needD3Fix = false; cpe_extEntityPos = false; cpe_twoWayPing = false; cpe_fastMap = false;
	game.SupportsCPEBlocks = false;
	NetworkProcessor net = (NetworkProcessor)game.Server;
	net.Reset();
}

	public static string[] ClientExtensions = new string[]{
		"ClickDistance", "CustomBlocks", "HeldBlock", "EmoteFix", "TextHotKey", "ExtPlayerList",
		"EnvColors", "SelectionCuboid", "BlockPermissions", "ChangeModel", "EnvMapAppearance",
		"EnvWeatherType", "MessageTypes", "HackControl", "PlayerClick", "FullCP437", "LongerMessages",
		"BlockDefinitions", "BlockDefinitionsExt", "BulkBlockUpdate", "TextColors", "EnvMapAspect",
		"EntityProperty", "ExtEntityPositions", "TwoWayPing", "InventoryOrder", "InstantMOTD", "FastMap",
	};

void CPEProtocol_Init() {
	Reset();
}

void CPEProtocol_Reset() {
	if (!Game_UseCPE) return;
	Set(OPCODE_CPE_EXT_INFO, HandleExtInfo, 67);
	Set(OPCODE_CPE_EXT_ENTRY, HandleExtEntry, 69);
	Set(OPCODE_CPE_SET_CLICK_DISTANCE, HandleSetClickDistance, 3);
	Set(OPCODE_CPE_CUSTOM_BLOCK_SUPPORT_LEVEL, HandleCustomBlockSupportLevel, 2);
	Set(OPCODE_CPE_HOLD_THIS, HandleHoldThis, 3);
	Set(OPCODE_CPE_SET_TEXT_HOTKEY, HandleSetTextHotkey, 134);

	Set(OPCODE_CPE_EXT_ADD_PLAYER_NAME, HandleExtAddPlayerName, 196);
	Set(OPCODE_CPE_EXT_ADD_ENTITY, HandleExtAddEntity, 130);
	Set(OPCODE_CPE_EXT_REMOVE_PLAYER_NAME, HandleExtRemovePlayerName, 3);

	Set(OPCODE_CPE_ENV_SET_COLOR, HandleEnvColours, 8);
	Set(OPCODE_CPE_MAKE_SELECTION, HandleMakeSelection, 86);
	Set(OPCODE_CPE_REMOVE_SELECTION, HandleRemoveSelection, 2);
	Set(OPCODE_CPE_SET_BLOCK_PERMISSION, HandleSetBlockPermission, 4);
	Set(OPCODE_CPE_SET_MODEL, HandleChangeModel, 66);
	Set(OPCODE_CPE_ENV_SET_MAP_APPERANCE, HandleEnvSetMapAppearance, 69);
	Set(OPCODE_CPE_ENV_SET_WEATHER, HandleEnvWeatherType, 2);
	Set(OPCODE_CPE_HACK_CONTROL, HandleHackControl, 8);
	Set(OPCODE_CPE_EXT_ADD_ENTITY2, HandleExtAddEntity2, 138);

	Set(OPCODE_CPE_BULK_BLOCK_UPDATE, HandleBulkBlockUpdate, 1282);
	Set(OPCODE_CPE_SET_TEXT_COLOR, HandleSetTextColor, 6);
	Set(OPCODE_CPE_ENV_SET_MAP_URL, HandleSetMapEnvUrl, 65);
	Set(OPCODE_CPE_ENV_SET_MAP_PROPERTY, HandleSetMapEnvProperty, 6);
	Set(OPCODE_CPE_SET_ENTITY_PROPERTY, HandleSetEntityProperty, 7);
	Set(OPCODE_CPE_TWO_WAY_PING, HandleTwoWayPing, 4);
	Set(OPCODE_CPE_SET_INVENTORY_ORDER, HandleSetInventoryOrder, 3);
}


void HandleExtInfo(Stream* stream) {
	UInt8 appNameBuffer[String_BufferSize(STRING_SIZE)];
	String appName = Handlers_ReadString(stream, appNameBuffer);
	game.Chat.Add("Server software: " + appName);

	String d3Server = String_FromConst("D3 server");
	if (String_CaselessStarts(&appName, &d3Server)) {
		cpe_needD3Fix = true;
	}

	/* Workaround for old MCGalaxy that send ExtEntry sync but ExtInfo async. This means
	   ExtEntry may sometimes arrive before ExtInfo, thus have to use += instead of = */
	cpe_serverExtensionsCount += Stream_ReadI16_BE(stream);
	SendCpeExtInfoReply();
}

void HandleExtEntry(Stream* stream) {
	UInt8 extNameBuffer[String_BufferSize(STRING_SIZE)];
	String extName = Handlers_ReadString(stream, extNameBuffer);
	Int32 extVersion = Stream_ReadI32_BE(stream);
	Platform_Log2("cpe ext: %s, %i", &extName, &extVersion);

	cpe_serverExtensionsCount--;
	SendCpeExtInfoReply();	

	/* update support state */
	if (ext == "HeldBlock") {
		cpe_sendHeldBlock = true;
	} else if (ext == "MessageTypes") {
		cpe_useMessageTypes = true;
	} else if (ext == "ExtPlayerList") {
		ServerConnection_SupportsExtPlayerList = true;
	} else if (ext == "BlockPermissions") {
		cpe_blockPerms = true;
	} else if (ext == "PlayerClick") {
		ServerConnection_SupportsPlayerClick = true;
	} else if (ext == "EnvMapAppearance") {
		cpe_envMapVer = extVersion;
		if (extVersion == 1) return;
		net.packetSizes[Opcode.CpeEnvSetMapApperance] += 4;
	} else if (ext == "LongerMessages") {
		ServerConnection_SupportsPartialMessages = true;
	} else if (ext == "FullCP437") {
		ServerConnection_SupportsFullCP437 = true;
	} else if (ext == "BlockDefinitionsExt") {
		cpe_blockDefsExtVer = extVersion;
		if (extVersion == 1) return;
		net.packetSizes[Opcode.CpeDefineBlockExt] += 3;
	} else if (ext == "ExtEntityPositions") {
		net.packetSizes[Opcode.EntityTeleport] += 6;
		net.packetSizes[Opcode.AddEntity] += 6;
		net.packetSizes[Opcode.CpeExtAddEntity2] += 6;
		cpe_extEntityPos = true;
	} else if (ext == "TwoWayPing") {
		cpe_twoWayPing = true;
	} else if (ext == "FastMap") {
		net.packetSizes[OPCODE_LEVEL_INIT] += 4;
		cpe_fastMap = true;
	}
}

void HandleSetClickDistance(Stream* stream) {
	LocalPlayer_Instance.ReachDistance = Stream_ReadU16_BE(stream) / 32.0f;
}

void HandleCustomBlockSupportLevel(Stream* stream) {
	UInt8 supportLevel = Stream_ReadU8(stream);
	stream = ServerConnection_WriteStream();
	WriteCustomBlockSupportLevel(stream, 1);
	ServerConnection_SendPacket();
	game.SupportsCPEBlocks = true;
	game.Events.RaiseBlockPermissionsChanged();
}

void HandleHoldThis(Stream* stream) {
	BlockID block  = Handlers_ReadBlock(stream);
	bool canChange = Stream_ReadU8(stream) == 0;

	Inventory_CanChangeHeldBlock = true;
	Inventory_SetSelectedBlock(block);
	Inventory_CanChangeHeldBlock = canChange;
	Inventory_CanPick = block != BLOCK_AIR;
}

void HandleSetTextHotkey(Stream* stream) {
	UInt8 labelBuffer[String_BufferSize(STRING_SIZE)];
	String label = Handlers_ReadString(stream, labelBuffer);
	UInt8 actionBuffer[String_BufferSize(STRING_SIZE)];
	String action = Handlers_ReadString(stream, actionBuffer);

	Int32 keyCode = Stream_ReadI32_BE(stream);
	UInt8 keyMods = Stream_ReadU8(stream);
	if (keyCode < 0 || keyCode > 255) return;

	Key key = Hotkeys_LWJGL[keyCode];
	if (key == Key_None) return;
	Platform_Log3("CPE hotkey added: %c, %b: %s", Key_Names[key], &keyMods, &action);

	if (action.length == 0) {
		Hotkeys_Remove(key, keyMods);
	} else if (action.buffer[action.length - 1] == '\n') {
		action = String_UNSAFE_Substring(&action, 0, action.length - 1);
		Hotkeys_Add(key, keyMods, &action, false);
	} else { /* more input needed by user */
		Hotkeys_Add(key, keyMods, &action, true);
	}
}

void HandleExtAddPlayerName(Stream* stream) {
	int id = Stream_ReadI16_BE(stream) & 0xFF;
	string playerName = Utils.StripColours(reader.ReadString());
	playerName = Utils.RemoveEndPlus(playerName);
	string listName = reader.ReadString();
	listName = Utils.RemoveEndPlus(listName);
	string groupName = reader.ReadString();
	UInt8 groupRank = Stream_ReadU8(stream);

	// Some server software will declare they support ExtPlayerList, but send AddEntity then AddPlayerName
	// we need to workaround this case by removing all the tab names we added for the AddEntity packets
	net.DisableAddEntityHack();
	net.AddTablistEntry((byte)id, playerName, listName, groupName, groupRank);
}

void HandleExtAddEntity(Stream* stream) {
	UInt8 id = Stream_ReadU8(stream);
	string displayName = reader.ReadString();
	string skinName = reader.ReadString();
	net.CheckName(id, ref displayName, ref skinName);
	net.AddEntity(id, displayName, skinName, false);
}

void HandleExtRemovePlayerName(Stream* stream) {
	int id = Stream_ReadI16_BE(stream) & 0xFF;
	net.RemoveTablistEntry((byte)id);
}

void HandleMakeSelection(Stream* stream) {
	UInt8 selectionId = Stream_ReadU8(stream);
	UInt8 labelBuffer[String_BufferSize(STRING_SIZE)];
	String label = Handlers_ReadString(stream, labelBuffer);

	Vector3I p1;
	p1.X = Stream_ReadI16_BE(stream);
	p1.Y = Stream_ReadI16_BE(stream);
	p1.Z = Stream_ReadI16_BE(stream);

	Vector3I p2;
	p2.X = Stream_ReadI16_BE(stream);
	p2.Y = Stream_ReadI16_BE(stream);
	p2.Z = Stream_ReadI16_BE(stream);

	PackedCol col;
	col.R = (UInt8)Stream_ReadI16_BE(stream);
	col.G = (UInt8)Stream_ReadI16_BE(stream);
	col.B = (UInt8)Stream_ReadI16_BE(stream);
	col.A = (UInt8)Stream_ReadI16_BE(stream);

	Selections_Add(selectionId, p1, p2, col);
}

void HandleRemoveSelection(Stream* stream) {
	UInt8 selectionId = Stream_ReadU8(stream);
	Selections_Remove(selectionId);
}

void HandleEnvColours(Stream* stream) {
	UInt8 variable = Stream_ReadU8(stream);
	Int16 r = Stream_ReadI16_BE(stream);
	Int16 g = Stream_ReadI16_BE(stream);
	Int16 b = Stream_ReadI16_BE(stream);
	bool invalid = r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255;
	PackedCol col = PACKEDCOL_CONST((UInt8)r, (UInt8)g, (UInt8)b, 255);

	if (variable == 0) {
		WorldEnv_SetSkyCol(invalid ? WorldEnv_DefaultSkyCol : col);
	} else if (variable == 1) {
		WorldEnv_SetCloudsCol(invalid ? WorldEnv_DefaultCloudsCol : col);
	} else if (variable == 2) {
		WorldEnv_SetFogCol(invalid ? WorldEnv_DefaultFogCol : col);
	} else if (variable == 3) {
		WorldEnv_SetShadowCol(invalid ? WorldEnv_DefaultShadowCol : col);
	} else if (variable == 4) {
		WorldEnv_SetSunCol(invalid ? WorldEnv_DefaultSunCol : col);
	}
}

void HandleSetBlockPermission(Stream* stream) {
	BlockID block = Handlers_ReadBlock(stream);
	Block_CanPlace[block]  = Stream_ReadU8(stream) != 0;
	Block_CanDelete[block] = Stream_ReadU8(stream) != 0;
	Event_RaiseVoid(&BlockEvents_PermissionsChanged);
}

void HandleChangeModel(Stream* stream) {
	UInt8 id = Stream_ReadU8(stream);
	UInt8 modelNameBuffer[String_BufferSize(STRING_SIZE)];
	String modelName = Handlers_ReadString(stream, &modelNameBuffer);

	String_MakeLowercase(&modelName);
	Entity* entity = Entities_List[id];
	if (entity != NULL) { Entity_SetModel(entity, &modelName); }
}

void HandleEnvSetMapAppearance(Stream* stream) {
	HandleSetMapEnvUrl(stream);
	WorldEnv_SetSidesBlock(Stream_ReadU8(stream));
	WorldEnv_SetEdgeBlock(Stream_ReadU8(stream));
	WorldEnv_SetEdgeHeight(Stream_ReadI16_BE(stream));
	if (cpe_envMapVer == 1) return;

	/* Version 2 */
	WorldEnv_SetCloudsHeight(Stream_ReadI16_BE(stream));
	Int16 maxViewDist = Stream_ReadI16_BE(stream);
	Game_MaxViewDistance = maxViewDist <= 0 ? 32768 : maxViewDist;
	Game_SetViewDistance(Game_UserViewDistance, false);
}

void HandleEnvWeatherType(Stream* stream) {
	WorldEnv_SetWeather(Stream_ReadU8(stream));
}

void HandleHackControl(Stream* stream) {
	LocalPlayer* p = &LocalPlayer_Instance;
	p->Hacks.CanFly                  = Stream_ReadU8(stream) != 0;
	p->Hacks.CanNoclip               = Stream_ReadU8(stream) != 0;
	p->Hacks.CanSpeed                = Stream_ReadU8(stream) != 0;
	p->Hacks.CanRespawn              = Stream_ReadU8(stream) != 0;
	p->Hacks.CanUseThirdPersonCamera = Stream_ReadU8(stream) != 0;
	LocalPlayer_CheckHacksConsistency();

	UInt16 jumpHeight = Stream_ReadU16_BE(stream);
	PhysicsComp* physics = &p->Physics;
	if (jumpHeight == UInt16_MaxValue) { /* special value of -1 to reset default */
		physics->JumpVel = HacksComp_CanJumpHigher(&p->Hacks) ? physics->UserJumpVel : 0.42f;
	} else {
		PhysicsComp_CalculateJumpVelocity(physics, jumpHeight / 32.0f);
	}

	physics->ServerJumpVel = physics->JumpVel;
	Event_RaiseVoid(&UserEvents_HackPermissionsChanged);
}

void HandleExtAddEntity2(Stream* stream) {
	UInt8 id = Stream_ReadU8(stream);
	string displayName = reader.ReadString();
	string skinName = reader.ReadString();
	net.CheckName(id, ref displayName, ref skinName);
	net.AddEntity(id, displayName, skinName, true);
}

#define BULK_MAX_BLOCKS 256
void HandleBulkBlockUpdate(Stream* stream) {
	Int32 i, count = Stream_ReadU8(stream) + 1;

	Int32 indices[BULK_MAX_BLOCKS];
	for (i = 0; i < count; i++) {
		indices[i] = Stream_ReadI32_BE(stream);
	}
	Stream_Skip(stream, (BULK_MAX_BLOCKS - count) * (UInt32)sizeof(Int32));

	BlockID blocks[BULK_MAX_BLOCKS];
	for (i = 0; i < count; i++) {
		blocks[i] = reader.buffer[reader.index + i];
	}
	reader.Skip(bulkCount);

	Int32 x, y, z;
	for (i = 0; i < count; i++) {
		Int32 index = indices[i];
		if (index < 0 || index >= World_BlocksSize) continue;
		World_Unpack(index, x, y, z);

		if (World_IsValidPos(x, y, z)) {
			Game_UpdateBlock(x, y, z, blocks[i]);
		}
	}
}

void HandleSetTextColor(Stream* stream) {
	PackedCol col;
	col.R = Stream_ReadU8(stream);
	col.G = Stream_ReadU8(stream);
	col.B = Stream_ReadU8(stream);
	col.A = Stream_ReadU8(stream);

	UInt8 code = Stream_ReadU8(stream);
	/* disallow space, null, and colour code specifiers */
	if (code == '\0' || code == ' ' || code == 0xFF) return;
	if (code == '%' || code == '&') return;

	Drawer2D_Cols[code] = col;
	Event_RaiseInt32(&ChatEvents_ColCodeChanged, code);
}

void HandleSetMapEnvUrl(Stream* stream) {
	UInt8 urlBuffer[String_BufferSize(STRING_SIZE)];
	String url = Handlers_ReadString(stream, urlBuffer);
	if (!Game_AllowServerTextures) return;

	if (url.length == 0) {
		/* don't extract default texture pack if we can */
		if (World_TextureUrl.length > 0) TexturePack_ExtractDefault();
	} else if (Utils_IsUrlPrefix(&url, 0)) {
		ServerConnection_RetrieveTexturePack(&url);
	}
	Platform_Log1("Image url: %s", &url);
}

void HandleSetMapEnvProperty(Stream* stream) {
	UInt8 type = Stream_ReadU8(stream);
	Int32 value = Stream_ReadI32_BE(stream);
	Math_Clamp(value, -0xFFFFFF, 0xFFFFFF);
	Int32 maxBlock = BLOCK_COUNT - 1;

	switch (type) {
	case 0:
		Math_Clamp(value, 0, maxBlock);
		WorldEnv_SetSidesBlock((BlockID)value); break;
	case 1:
		Math_Clamp(value, 0, maxBlock);
		WorldEnv_SetEdgeBlock((BlockID)value); break;
	case 2:
		WorldEnv_SetEdgeHeight(value); break;
	case 3:
		WorldEnv_SetCloudsHeight(value); break;
	case 4:
		Math_Clamp(value, -0x7FFF, 0x7FFF);
		Game_MaxViewDistance = value <= 0 ? 32768 : value;
		Game_SetViewDistance(Game_UserViewDistance, false); break;
	case 5:
		WorldEnv_SetCloudsSpeed(value / 256.0f); break;
	case 6:
		WorldEnv_SetWeatherSpeed(value / 256.0f); break;
	case 7:
		Math_Clamp(value, 0, UInt8_MaxValue);
		WorldEnv_SetWeatherFade(value / 128.0f); break;
	case 8:
		WorldEnv_SetExpFog(value != 0); break;
	case 9:
		WorldEnv_SetSidesOffset(value); break;
	case 10:
		WorldEnv_SetSkyboxHorSpeed(value / 1024.0f); break;
	case 11:
		WorldEnv_SetSkyboxVerSpeed(value / 1024.0f); break;
	}
}

void HandleSetEntityProperty(Stream* stream) {
	UInt8 id = Stream_ReadU8(stream);
	UInt8 type = Stream_ReadU8(stream);
	Int32 value = Stream_ReadI32_BE(stream);

	Entity* entity = Entities_List[id];
	if (entity == NULL) return;
	LocationUpdate update; LocationUpdate_Empty(&update);

	Real32 scale;
	switch (type) {
		update.Flags |= LOCATIONUPDATE_FLAG_ROTX;
		update.RotX = LocationUpdate_Clamp(value); break;
	case 1:
		update.Flags |= LOCATIONUPDATE_FLAG_HEADY;
		update.HeadY = LocationUpdate_Clamp(value); break;
	case 2:
		update.Flags |= LOCATIONUPDATE_FLAG_ROTZ;
		update.RotZ = LocationUpdate_Clamp(value); break;

	case 3:
	case 4:
	case 5:
		scale = value / 1000.0f;
		Math_Clamp(scale, 0.01f, entity->Model->MaxScale);
		if (type == 3) entity->ModelScale.X = scale;
		if (type == 4) entity->ModelScale.Y = scale;
		if (type == 5) entity->ModelScale.Z = scale;

		Entity_UpdateModelBounds(entity);
		return;
	default:
		return;
	}
	entity->VTABLE->SetLocation(entity, &update, true);
}

void HandleTwoWayPing(Stream* stream) {
	bool serverToClient = Stream_ReadU8(stream) != 0;
	UInt16 data = Stream_ReadU16_BE(stream);
	if (!serverToClient) { PingList_Update(data); return; }

	stream = ServerConnection_WriteStream();
	WriteTwoWayPing(stream, true, data); /* server to client reply */
	ServerConnection_SendPacket();
}

void HandleSetInventoryOrder(Stream* stream) {
	BlockID block = Handlers_ReadBlock(stream);
	BlockID order = Handlers_ReadBlock(stream);

	Inventory_Remove(block);
	if (order != 255 && order != 0) {
		Inventory_Map[order - 1] = block;
	}
}

#define Ext_Deg2Packed(x) ((Int16)((x) * 65536.0f / 360.0f))
void WritePlayerClick(Stream* stream, MouseButton button, bool buttonDown, UInt8 targetId, PickedPos pos) {
	Entity* p = &LocalPlayer_Instance.Base;
	Stream_WriteU8(stream, OPCODE_CPE_PLAYER_CLICK);
	Stream_WriteU8(stream, button);
	Stream_WriteU8(stream, buttonDown ? 0 : 1);
	Stream_WriteI16_BE(stream, Ext_Deg2Packed(p->HeadY));
	Stream_WriteI16_BE(stream, Ext_Deg2Packed(p->HeadX));

	Stream_WriteU8(stream, targetId);
	Stream_WriteI16_BE(stream, pos.BlockPos.X);
	Stream_WriteI16_BE(stream, pos.BlockPos.Y);
	Stream_WriteI16_BE(stream, pos.BlockPos.Z);

	UInt8 face = 255;
	/* Our own face values differ from CPE block face */
	switch (pos.ClosestFace) {
	case FACE_XMAX: face = 0; break;
	case FACE_XMIN: face = 1; break;
	case FACE_YMAX: face = 2; break;
	case FACE_YMIN: face = 3; break;
	case FACE_ZMAX: face = 4; break;
	case FACE_ZMIN: face = 5; break;
	}
	Stream_WriteU8(stream, face);
}

void WriteExtInfo(Stream* stream, STRING_PURE String* appName, Int32 extensionsCount) {
	Stream_WriteU8(stream, OPCODE_CPE_EXT_INFO);
	Handlers_WriteString(stream, appName);
	Stream_WriteU16_BE(stream, extensionsCount);
}

void WriteExtEntry(Stream* stream, STRING_PURE String* extensionName, Int32 extensionVersion) {
	Stream_WriteU8(stream, OPCODE_CPE_EXT_ENTRY);
	Handlers_WriteString(stream, extensionName);
	Stream_WriteI32_BE(stream, extensionVersion);
}

void WriteCustomBlockSupportLevel(Stream* stream, UInt8 version) {
	Stream_WriteU8(stream, OPCODE_CPE_CUSTOM_BLOCK_SUPPORT_LEVEL);
	Stream_WriteU8(stream, version);
}

void WriteTwoWayPing(Stream* stream, bool serverToClient, UInt16 data) {
	Stream_WriteU8(stream, OPCODE_CPE_TWO_WAY_PING);
	Stream_WriteU8(stream, serverToClient ? 1 : 0);
	Stream_WriteU16_BE(stream, data);
}

void SendCpeExtInfoReply() {
	if (cpe_serverExtensionsCount != 0) return;
	string[] clientExts = CPESupport.ClientExtensions;
	int count = clientExts.Length;
	if (!Game_AllowCustomBlocks) count -= 2;

	WriteExtInfo(net.AppName, count);
	ServerConnection_SendPacket();
	for (int i = 0; i < clientExts.Length; i++) {
		string name = clientExts[i];
		int ver = 1;
		if (name == "ExtPlayerList") ver = 2;
		if (name == "EnvMapAppearance") ver = cpe_envMapVer;
		if (name == "BlockDefinitionsExt") ver = cpe_blockDefsExtVer;

		if (!Game_AllowCustomBlocks && name == "BlockDefinitionsExt") continue;
		if (!Game_AllowCustomBlocks && name == "BlockDefinitions")    continue;

		WriteExtEntry(name, ver);
		ServerConnection_SendPacket();
	}
}


/*########################################################################################################################*
*------------------------------------------------------Custom blocks------------------------------------------------------*
*#########################################################################################################################*/
void BlockDefs_Init() { BlockDefs_Reset(); }

void BlockDefs_Reset() {
	if (!Game_UseCPE || !Game_AllowCustomBlocks) return;
	Set(OPCODE_CPE_DEFINE_BLOCK, HandleDefineBlock, 80);
	Set(OPCODE_CPE_UNDEFINE_BLOCK, HandleRemoveBlockDefinition, 2);
	Set(OPCODE_CPE_DEFINE_BLOCK_EXT, HandleDefineBlockExt, 85);
}

void HandleDefineBlock(Stream* stream) {
	BlockID block = HandleDefineBlockCommonStart(stream, false);

	UInt8 shape = Stream_ReadU8(stream);
	if (shape > 0 && shape <= 16) {
		Block_MaxBB[block].Y = shape / 16.0f;
	}

	HandleDefineBlockCommonEnd(stream, shape, block);
	/* Update sprite BoundingBox if necessary */
	if (Block_Draw[block] == DRAW_SPRITE) {
		Block_RecalculateBB(block);
	}
}

void HandleRemoveBlockDefinition(Stream* stream) {
	BlockID block = Handlers_ReadBlock(stream);
	bool didBlockLight = Block_BlocksLight[block];

	Bloc_ResetBlockProps(block);
	OnBlockUpdated(block, didBlockLight);
	Block_UpdateCulling(block);

	Inventory_Remove(block);
	if (block < BLOCK_CPE_COUNT) { Inventory_AddDefault(block); }

	Block_SetCustomDefined(block, false);
	Event_RaiseVoid(&BlockEvents_BlockDefChanged);
}

void OnBlockUpdated(BlockID block, bool didBlockLight) {
	if (World_Blocks == NULL) return;
	/* Need to refresh lighting when a block's light blocking state changes */
	if (Block_BlocksLight[block] != didBlockLight) { Lighting_Refresh(); }
}

void HandleDefineBlockExt(Stream* stream) {
	BlockID block = HandleDefineBlockCommonStart(stream, cpe_blockDefsExtVer >= 2);
	Vector3 min, max;

	min.X = Stream_ReadU8(stream) / 16.0f; Utils.Clamp(ref min.X, 0.0f, 15.0f / 16.0f);
	min.Y = Stream_ReadU8(stream) / 16.0f; Utils.Clamp(ref min.Y, 0.0f, 15.0f / 16.0f);
	min.Z = Stream_ReadU8(stream) / 16.0f; Utils.Clamp(ref min.Z, 0.0f, 15.0f / 16.0f);
	max.X = Stream_ReadU8(stream) / 16.0f; Utils.Clamp(ref max.X, 1.0f / 16.0f, 1.0f);
	max.Y = Stream_ReadU8(stream) / 16.0f; Utils.Clamp(ref max.Y, 1.0f / 16.0f, 1.0f);
	max.Z = Stream_ReadU8(stream) / 16.0f; Utils.Clamp(ref max.Z, 1.0f / 16.0f, 1.0f);

	Block_MinBB[block] = min;
	Block_MaxBB[block] = max;
	HandleDefineBlockCommonEnd(stream, 1, block);
}

BlockID HandleDefineBlockCommonStart(Stream* stream, bool uniqueSideTexs) {
	BlockID block = Handlers_ReadBlock(stream);
	bool didBlockLight = Block_BlocksLight[block];
	Block_ResetBlockProps(block);

	UInt8 nameBuffer[String_BufferSize(STRING_SIZE)];
	String name = Handlers_ReadString(stream, nameBuffer);
	Block_SetName(block, &name);
	Block_SetCollide(block, Stream_ReadU8(stream));

	Block_SpeedMultiplier[block] = (float)Math.Pow(2, (Stream_ReadU8(stream) - 128) / 64.0f);
	Block_SetTex(Stream_ReadU8(stream), FACE_YMAX, block);
	if (uniqueSideTexs) {
		Block_SetTex(Stream_ReadU8(stream), FACE_XMIN, block);
		Block_SetTex(Stream_ReadU8(stream), FACE_XMAX, block);
		Block_SetTex(Stream_ReadU8(stream), FACE_ZMIN, block);
		Block_SetTex(Stream_ReadU8(stream), FACE_ZMAX, block);
	} else {
		Block_SetSide(Stream_ReadU8(stream), block);
	}
	Block_SetTex(Stream_ReadU8(stream), FACE_YMIN, block);

	Block_BlocksLight[block] = Stream_ReadU8(stream) == 0;
	OnBlockUpdated(block, didBlockLight);

	UInt8 sound = Stream_ReadU8(stream);
	Block_StepSounds[block] = sound;
	Block_DigSounds[block] = sound;
	if (sound == SOUND_GLASS) Block_StepSounds[block] = SOUND_STONE;

	Block_FullBright[block] = Stream_ReadU8(stream) != 0;
	return block;
}

void HandleDefineBlockCommonEnd(Stream* stream, UInt8 shape, BlockID block) {
	UInt8 blockDraw = Stream_ReadU8(stream);
	if (shape == 0) {
		Block_SpriteOffset[block] = blockDraw;
		blockDraw = DRAW_SPRITE;
	}
	UInt8 fogDensity = Stream_ReadU8(stream);
	Block_FogDensity[block] = fogDensity == 0 ? 0 : (fogDensity + 1) / 128.0f;

	PackedCol col; col.A = 255;
	col.R = Stream_ReadU8(stream);
	col.G = Stream_ReadU8(stream);
	col.B = Stream_ReadU8(stream);	
	Block_FogCol[block] = col;
	Block_DefineCustom(block);
}

#if FALSE
void HandleDefineModel() {
	int start = reader.index - 1;
	UInt8 id = Stream_ReadU8(stream);
	CustomModel model = null;

	switch (Stream_ReadU8(stream)) {
	case 0:
		model = new CustomModel(game);
		model.ReadSetupPacket(reader);
		game.ModelCache.CustomModels[id] = model;
		break;
	case 1:
		model = game.ModelCache.CustomModels[id];
		if (model != null) model.ReadMetadataPacket(reader);
		break;
	case 2:
		model = game.ModelCache.CustomModels[id];
		if (model != null) model.ReadDefinePartPacket(reader);
		break;
	case 3:
		model = game.ModelCache.CustomModels[id];
		if (model != null) model.ReadRotationPacket(reader);
		break;
	}
	int total = packetSizes[(byte)Opcode.CpeDefineModel];
	reader.Skip(total - (reader.index - start));
}
#endif


/*########################################################################################################################*
*------------------------------------------------------WoM protocol-------------------------------------------------------*
*#########################################################################################################################*/
/* Partially based on information from http://files.worldofminecraft.com/texturing/ */
/* NOTE: http://files.worldofminecraft.com/ has been down for quite a while, so support was removed on Oct 10, 2015 */

UInt8 wom_identifierBuffer[String_BufferSize(STRING_SIZE)];
String wom_identifier = String_FromEmptyArray(wom_identifierBuffer);
Int32 wom_counter;
bool wom_sendId, wom_sentId;

void WoM_UpdateIdentifier() {
	String_Clear(&wom_identifier);
	String_AppendConst(&wom_identifier, "womenv_");
	String_AppendInt32(&wom_identifier, wom_counter);
}

void WoM_Reset() {
	wom_counter = 0;
	WoM_UpdateIdentifier();
	wom_sendId = false; wom_sentId = false;
}

void WoM_Tick() {
	AsyncRequest item;
	bool success = AsyncDownloader_Get(&wom_identifier, &item);
	if (success && item.ResultString.length > 0) {
		ParseWomConfig(&item.ResultString);
		Platform_MemFree(&item.ResultString.buffer);
	}
}

void WoM_CheckMotd() {
	String motd = ServerConnection_ServerMOTD;
	if (motd.length == 0) return;

	String cfg = String_FromConst("cfg=");
	Int32 index = String_IndexOfString(&motd, &cfg);
	if (Game_PureClassic || index == -1) return;

	string host = String_UNSAFE_SubstringAt(&motd, index + cfg.length);
	string url = "http://" + host;
	url = url.Replace("$U", game.Username);

	/* Ensure that if the user quickly changes to a different world, env settings from old world aren't 
	applied in the new world if the async 'get env request' didn't complete before the old world was unloaded */
	wom_counter++;
	WoM_UpdateIdentifier();
	AsyncDownloader_Download(&url, true, REQUEST_TYPE_STRING, &wom_identifier);
	wom_sendId = true;
}

void WoM_CheckSendWomID() {
	if (wom_sendId && !wom_sentId) {
		String msg = String_FromConst("/womid WoMClient-2.0.7")
		ServerConnection_SendChat(&msg);
		wom_sentId = true;
	}
}

PackedCol WoM_ParseCol(STRING_PURE String* value, PackedCol defaultCol) {
	Int32 argb;
	if (!Convert_TryParseInt32(value, &argb)) return defaultCol;

	PackedCol col; col.A = 255;
	col.R = (UInt8)(argb >> 16);
	col.G = (UInt8)(argb >> 8);
	col.B = (UInt8)argb;
	return col;
}

bool WoM_ReadLine(STRING_REF String* page, Int32* start, STRING_TRANSIENT String* line) {
	Int32 i, offset = *start;
	if (offset == -1) return false;

	for (i = offset; i < page->length; i++) {
		UInt8 c = page->buffer[i];
		if (c != '\r' && c != '\n') continue;

		*line = String_UNSAFE_Substring(page, offset, i - offset);
		offset = i + 1;
		if (c == '\r' && offset < page->length && page->buffer[offset] == '\n') {
			offset++; /* we stop at the \r, so make sure to skip following \n */
		}

		start = offset; return true;
	}

	*line = String_UNSAFE_SubstringAt(page, offset);
	start = -1;
	return true;
}

void Wom_ParseConfig(STRING_PURE String* page) {
	String line;
	Int32 start = 0;

	while (WoM_ReadLine(page, &start, &line)) {
		Platform_Log(&line);
		Int32 sepIndex = String_IndexOf(&line, '=', 0);
		if (sepIndex == -1) continue;

		String key = String_UNSAFE_Substring(&line, 0, sepIndex);
		String_UNSAFE_TrimEnd(&key);
		String value = String_UNSAFE_SubstringAt(&line, sepIndex + 1);
		String_UNSAFE_TrimStart(&value);

		if (String_CaselessEqualsConst(&key, "environment.cloud")) {
			PackedCol col = WoM_ParseCol(&value, WorldEnv_DefaultCloudsCol);
			WorldEnv_SetCloudsCol(col);
		} else if (String_CaselessEqualsConst(&key, "environment.sky")) {
			PackedCol col = WoM_ParseCol(&value, WorldEnv_DefaultSkyCol);
			WorldEnv_SetSkyCol(col);
		} else if (String_CaselessEqualsConst(&key, "environment.fog")) {
			PackedCol col = WoM_ParseCol(&value, WorldEnv_DefaultFogCol);
			WorldEnv_SetFogCol(col);
		} else if (String_CaselessEqualsConst(&key, "environment.level")) {
			Int32 waterLevel;
			if (Convert_TryParseInt32(&value, &waterLevel)) {
				WorldEnv_SetEdgeHeight(waterLevel);
			}
		} else if (String_CaselessEqualsConst(&key, "user.detail") && !cpe_useMessageTypes) {
			Chat_AddOf(&value, MSG_TYPE_STATUS_2);
		}
	}
}