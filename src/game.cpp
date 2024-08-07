#include "core/crt.h"
#include "core/log.h"
#include "core/math.h"
#include "core/os.h"
#include "core/path.h"
#include "core/stream.h"
#include "engine/engine.h"
#include "engine/input_system.h"
#include "engine/plugin.h"
#include "engine/prefab.h"
#include "engine/reflection.h"
#include "engine/resource_manager.h"
#include "engine/world.h"
#include "imgui/imgui.h"
#include "gui/gui_module.h"
#include "gui/gui_system.h"
#include "../plugins/net/src/net.h"
#include "renderer/particle_system.h"
#include "renderer/render_module.h"


using namespace Lumix;

static const ComponentType MODEL_INSTANCE_TYPE = reflection::getComponentType("model_instance");

struct Tile {
	enum Type {
		EMPTY,
		WALL,
		BLOCK,
		BOMB,
		UPGRADE_BOMB,
		UPGRADE_FLAME,
		UPGRADE_SPEED,

		COUNT
	};
	Type type;
	EntityPtr entity = INVALID_ENTITY;
	float countdown;
	u32 flame_size;
};


struct GameSystem : ISystem {
	GameSystem(Engine& engine);

	const char* getName() const override { return "myplugin"; }
	
	void serialize(OutputMemoryStream& serializer) const override {}
	bool deserialize(i32 version, InputMemoryStream& serializer) override {
		return version == 0;
	}

	void createModules(World& world) override;

	Engine& m_engine;
};


struct GameModule : IModule {
	GameModule(Engine& engine, GameSystem& system, World& world, IAllocator& allocator)
		: m_engine(engine)
		, m_system(system)
		, m_world(world)
		, m_allocator(allocator)
		, m_explosions(m_allocator)
	{}

	const char* getName() const override { return "myplugin"; }

	void serialize(struct OutputMemoryStream& serializer) override {
	}

	void deserialize(struct InputMemoryStream& serializer, const struct EntityMap& entity_map, i32 version) override {
	}

	ISystem& getSystem() const override { return m_system; }
	World& getWorld() override { return m_world; }
	
	void placeBomb() {
		if (m_player.free_bombs == 0) return;
		--m_player.free_bombs;

		IVec2 ipos(m_player.pos + 0.5f);

		Tile& t = m_board[ipos.x][ipos.y];
		t.type = Tile::BOMB;
		if (t.entity.isValid()) {
			destroyEntity(*t.entity);
		}

		EntityMap entity_map(m_engine.getAllocator());
		EntityPtr e = m_engine.instantiatePrefab(m_world, *m_tile_prefabs[Tile::BOMB], {(float)ipos.x, 0, (float)ipos.y}, Quat::IDENTITY, {1, 1, 1}, entity_map);
		t.entity = e;
		t.countdown = 2;
		t.flame_size = m_player.flame_size;
		t.type = Tile::BOMB;
	}

	void destroyEntity(EntityRef e) {
		for (;;) {
			EntityPtr c = m_world.getFirstChild(e);
			if (!c.isValid()) break;
			destroyEntity(*c);
		}
		m_world.destroyEntity(e);
	}

	void explode(u32 x, u32 y, u32 flame_size) {
		const u32 w = lengthOf(m_board);
		const u32 h = lengthOf(m_board[0]);

		IVec2 center(x, y);
		
		Tile& t = m_board[x][y];
		destroyEntity(*t.entity);
		t.entity = INVALID_ENTITY;
		t.type = Tile::EMPTY;
		++m_player.free_bombs;

		EntityMap entity_map(m_engine.getAllocator());
		auto flame_line = [&](IVec2 dir){
			for (u32 i = 1; i <= flame_size; ++i) {
				const IVec2 p = center + dir * i;
				if (p.x < 0 || p.y < 0) return;
				if (p.x >= i32(w) || p.y >= i32(h)) return;

				Tile& t = m_board[p.x][p.y];
				switch (t.type) {
					case Tile::BOMB:
						explode(p.x, p.y, t.flame_size);
						break;
					case Tile::WALL: 
						return;
					case Tile::BLOCK: {
						t.type = Tile::EMPTY;
						destroyEntity(*t.entity);
						t.entity = INVALID_ENTITY;
						const DVec3 pos { (float)p.x, 0, (float)p.y };
						EntityPtr e = m_engine.instantiatePrefab(m_world, *m_explosion_prefab, pos, Quat::IDENTITY, { 1, 1, 1 }, entity_map);
						if (e) m_explosions.push(*e);
						return;
					}
					default: {
						const DVec3 pos{(float)p.x, 0, (float)p.y};
						EntityPtr e = m_engine.instantiatePrefab(m_world, *m_explosion_prefab, pos, Quat::IDENTITY, { 1, 1, 1 }, entity_map);
						if (e) m_explosions.push(*e);
						break;
					}
				}
			}
		};

		const DVec3 pos{(float)center.x, 0, (float)center.y};
		EntityPtr e = m_engine.instantiatePrefab(m_world, *m_explosion_prefab, pos, Quat::IDENTITY, { 1, 1, 1 }, entity_map);
		if (e) m_explosions.push(*e);

		flame_line({1, 0});
		flame_line({-1, 0});
		flame_line({0, 1});
		flame_line({0, -1});
	}

	void update(float time_delta) {
		if (m_game_state == GameState::NOT_RUNNING) return;

		if (!m_player_prefab->isReady()) return;
		if (!m_explosion_prefab->isReady()) return;
		for (PrefabResource* res : m_tile_prefabs) {
			if (res && res->isEmpty()) return;
		}

		if (m_game_state == GameState::LOADING) {
			if (m_is_server) {
				initGame();
				m_game_state = GameState::RUNNING;
			}
			// client sets running state when it receives sync board message
		}

		if (m_is_server && m_to_client == NetSystem::INVALID_CONNECTION) return;
		if (!m_is_server && m_to_server == NetSystem::INVALID_CONNECTION) return;

		// client waits for sync board net msg
		if (m_game_state != GameState::RUNNING) return;

		// input events
		Span<const InputSystem::Event> events = m_engine.getInputSystem().getEvents();
		for (const InputSystem::Event& event : events) {
			switch (event.type) {
				case InputSystem::Event::BUTTON:
					if (event.device->type == InputSystem::Device::KEYBOARD) {
						switch((os::Keycode)event.data.button.key_id) {
							case os::Keycode::LEFT: m_left_input = event.data.button.down; if (event.data.button.down) m_vertical_prio = false; break;
							case os::Keycode::RIGHT: m_right_input = event.data.button.down; if (event.data.button.down) m_vertical_prio = false; break;
							case os::Keycode::UP: m_up_input = event.data.button.down; if (event.data.button.down) m_vertical_prio = true; break;
							case os::Keycode::DOWN: m_down_input = event.data.button.down; if (event.data.button.down) m_vertical_prio = true; break;
							case os::Keycode::SPACE: if (event.data.button.down) placeBomb(); break;
							default: break;
						}
					}
					break;
				default: break;
			}
		}

		// bombs
		const u32 w = lengthOf(m_board);
		const u32 h = lengthOf(m_board[0]);

		for (u32 i = 0; i < w; ++i) {
			for (u32 j = 0; j < h; ++j) {
				Tile& t = m_board[i][j];
				if (t.type != Tile::BOMB) continue;
				t.countdown -= time_delta;
				if (t.countdown <= 0) {
					explode(i, j, t.flame_size);
				}
			}
		}

		// movement
		struct {
			float val;
			float consume(float amount) {
				float tmp = minimum(fabsf(amount), val);
				val -= tmp;
				return tmp;
			}
		} step = {time_delta * m_player.speed};
		
		auto hmove = [&](int delta){
			m_player.orientation = delta > 0 ? Orientation::E : Orientation::W;
			IVec2 ipos = IVec2(m_player.pos + Vec2(0.5f, 0));
			IVec2 inext = ipos + IVec2(delta, 0);
			if (m_board[inext.x][inext.y].type == Tile::EMPTY) {
				m_player.pos.y -= step.consume(m_player.pos.y - ipos.y) * signum(m_player.pos.y - ipos.y);
			}
			else if (fabsf(m_player.pos.y - ipos.y) > 0.1f) {
				inext = inext + IVec2(0, ipos.y > m_player.pos.y ? -1 : 1); 
				if (m_board[inext.x][inext.y].type == Tile::EMPTY) {
					m_player.pos.y -= step.consume(m_player.pos.y - inext.y) * signum(m_player.pos.y - inext.y);
				}
			}
			m_player.pos.x += step.consume(getHSpace(delta)) * delta;
		};
		
		auto vmove = [&](int delta) {
			m_player.orientation = delta > 0 ? Orientation::S : Orientation::N;
			IVec2 ipos = IVec2(m_player.pos + Vec2(0, 0.5f));
			IVec2 inext = ipos + IVec2(0, delta);
			if (m_board[inext.x][inext.y].type == Tile::EMPTY) {
				m_player.pos.x -= step.consume(m_player.pos.x - ipos.x) * signum(m_player.pos.x - ipos.x);
			}
			else if (fabsf(m_player.pos.x - ipos.x) > 0.1f) {
				inext = inext + IVec2(ipos.x > m_player.pos.x ? -1 : 1, 0); 
				if (m_board[inext.x][inext.y].type == Tile::EMPTY) {
					m_player.pos.x -= step.consume(m_player.pos.x - inext.x) * signum(m_player.pos.x - inext.x);
				}
			}
			m_player.pos.y += step.consume(getVSpace(delta)) * delta;
		};

		if (m_vertical_prio) {
			if (m_down_input) vmove(1);
			if (m_up_input) vmove(-1);
			if (m_right_input) hmove(1);
			if (m_left_input) hmove(-1);
		}
		else {
			if (m_right_input) hmove(1);
			if (m_left_input) hmove(-1);
			if (m_down_input) vmove(1);
			if (m_up_input) vmove(-1);
		}

		m_world.setPosition(*m_player.entity, DVec3{m_player.pos.x, 0, m_player.pos.y});

		switch (m_player.orientation) {
			case Orientation::N: m_world.setRotation(*m_player.entity, Quat(Vec3(0, 1, 0), PI)); break;
			case Orientation::E: m_world.setRotation(*m_player.entity, Quat(Vec3(0, 1, 0), PI * 0.5f)); break;
			case Orientation::S: m_world.setRotation(*m_player.entity, Quat(Vec3(0, 1, 0), 0)); break;
			case Orientation::W: m_world.setRotation(*m_player.entity, Quat(Vec3(0, 1, 0), PI * 1.5f)); break;
		}

		// check if explosion finished
		RenderModule* render_module = (RenderModule*)m_world.getModule("renderer");
		for (i32 i = m_explosions.size() - 1; i >= 0; --i) {
			EntityPtr ps_entity = m_world.getFirstChild(m_explosions[i]);
			const ParticleSystem& ps = render_module->getParticleSystem(*ps_entity);
			if (ps.m_total_time == 0) continue;
			u32 sum = 0;
			for (const ParticleSystem::Emitter& emitter : ps.getEmitters()) sum += emitter.particles_count;
			if (sum == 0) {
				destroyEntity(m_explosions[i]);
				m_explosions.swapAndPop(i);
			}
		}
	}

	float getHSpace(int dir) const {
		float res = 0;
		IVec2 ipos(m_player.pos + 0.5f);
		if (dir * (ipos.x - m_player.pos.x) > 0) res += (ipos.x - m_player.pos.x) * dir;
		ipos.x += dir;
		if (m_board[ipos.x][ipos.y].type == Tile::EMPTY) res += 1;
		return res;
	}

	float getVSpace(int dir) const {
		float res = 0;
		IVec2 ipos(m_player.pos + 0.5f);
		if (dir * (ipos.y - m_player.pos.y) > 0) res += (ipos.y - m_player.pos.y) * dir;
		ipos.y += dir;
		if (m_board[ipos.x][ipos.y].type == Tile::EMPTY) res += 1;
		return res;
	}

	void startGame() override {
		ResourceManagerHub& rm = m_system.m_engine.getResourceManager();
		m_tile_prefabs[Tile::WALL] = rm.load<PrefabResource>(Path("prefabs/wall.fab"));
		m_tile_prefabs[Tile::BLOCK] = rm.load<PrefabResource>(Path("prefabs/block.fab"));
		m_tile_prefabs[Tile::EMPTY] = rm.load<PrefabResource>(Path("prefabs/ground.fab"));
		m_tile_prefabs[Tile::BOMB] = rm.load<PrefabResource>(Path("prefabs/bomb.fab"));
		m_player_prefab = rm.load<PrefabResource>(Path("prefabs/player.fab"));
		m_explosion_prefab = rm.load<PrefabResource>(Path("prefabs/explosion.fab"));
		m_game_state = GameState::LOADING;
		
		GUISystem& system = (GUISystem&)m_world.getModule("gui")->getSystem();
		system.enableCursor(true);

		NetSystem& net = getNetSystem();
		net.onDataReceived().bind<&GameModule::dataReceived>(this);
		net.onConnect().bind<&GameModule::clientConnected>(this);
		net.onDisconnect().bind<&GameModule::clientDisconnected>(this);
	}

	void dataReceived(NetSystem::ConnectionHandle connection, Span<const u8> data) {
		ASSERT(m_is_server && connection == m_to_client || !m_is_server && connection == m_to_server);

		InputMemoryStream blob(data);
		const NetMessageType msg_type = blob.read<NetMessageType>();
		switch (msg_type) {
			case NetMessageType::SYNC_BOARD:
				m_game_state = GameState::RUNNING;
				initGame((const Tile::Type*)blob.skip(1));
				logInfo("Sync board message received");
				break;
			default:
				logError("Unknown network message received");
				break;
		}
	}

	enum class NetMessageType : u8 {
		SYNC_BOARD
	};

	void syncBoard() {
		OutputMemoryStream blob(m_allocator);
		blob.reserve(1 + lengthOf(m_board) * lengthOf(m_board[0]) * sizeof(Tile::Type));
		blob.write(NetMessageType::SYNC_BOARD);
		const u32 w = lengthOf(m_board);
		const u32 h = lengthOf(m_board[0]);

		for (u32 i = 0; i < w; ++i) {
			for (u32 j = 0; j < h; ++j) {
				blob.write(m_board[i][j].type);
			}
		}

		getNetSystem().send(m_to_client, blob, true);
	}

	void clientConnected(NetSystem::ConnectionHandle connection) {
		if (!m_is_server) return;
		
		ASSERT(m_to_client == NetSystem::INVALID_CONNECTION);
		m_to_client = connection;
		logInfo("Client connected");
		syncBoard();
	}

	void clientDisconnected(NetSystem::ConnectionHandle connection) {
		if (!m_is_server) return;
		ASSERT(connection == m_to_client);
		m_to_client = NetSystem::INVALID_CONNECTION;
		logInfo("Client disconnected");
	}

	void initGame(const Tile::Type* init_data = nullptr) {
		const u32 w = lengthOf(m_board);
		const u32 h = lengthOf(m_board[0]);
		RenderModule* render_module = (RenderModule*)m_world.getModule("renderer");
		EntityMap entity_map(m_engine.getAllocator());

		if (init_data) {
			for (u32 i = 0; i < w; ++i) {
				for (u32 j = 0; j < h; ++j) {
					m_board[i][j].type = init_data[j + i * h];
				}
			}
		}
		else {
			for (u32 i = 0; i < w; ++i) {
				for (u32 j = 0; j < h; ++j) {
					m_board[i][j].type = Tile::EMPTY;
					if (i == 0 || j == 0 || i == w - 1 || j == h - 1) {
						m_board[i][j].type = Tile::WALL;
					}
					else if (i % 2 == 0 && j % 2 == 0) {
						m_board[i][j].type = Tile::WALL;
					}
					else {
						if (rand() % 2) {
							m_board[i][j].type = Tile::BLOCK;
						}
					}
				}
			}
			m_board[1][1].type = Tile::EMPTY;
			m_board[2][1].type = Tile::EMPTY;
			m_board[1][2].type = Tile::EMPTY;
		}

		for (u32 i = 0; i < w; ++i) {
			for (u32 j = 0; j < h; ++j) {
				const DVec3 pos{(float)i, 0, (float)j};
				m_engine.instantiatePrefab(m_world, *m_tile_prefabs[Tile::EMPTY], pos, Quat::IDENTITY, {1, 1, 1}, entity_map);
				const Tile::Type type = m_board[i][j].type;
				if (type != Tile::EMPTY) {
					EntityPtr e = m_engine.instantiatePrefab(m_world, *m_tile_prefabs[type], pos, Quat::IDENTITY, {1, 1, 1}, entity_map);
					m_board[i][j].entity = e;
				}
			}
		}

		m_player.entity = m_engine.instantiatePrefab(m_world, *m_player_prefab, {m_player.pos.x, 0, m_player.pos.y}, Quat::IDENTITY, {1, 1, 1}, entity_map);
	}

	void stopGame() override {
		m_game_state = GameState::NOT_RUNNING;

		for (PrefabResource*& res : m_tile_prefabs) {
			if (res) res->decRefCount();
			res = nullptr;
		}
		if (m_player_prefab) {
			m_player_prefab->decRefCount();
			m_player_prefab = nullptr;
		}
		if (m_explosion_prefab) {
			m_explosion_prefab->decRefCount();
			m_explosion_prefab = nullptr;
		}
	}

	NetSystem& getNetSystem() {
		return *(NetSystem*)m_engine.getSystemManager().getSystem("network");
	}

	void connectClicked(EntityRef e) {
		GUIModule* gui = (GUIModule*)m_world.getModule("gui");
		EntityRef parent = *m_world.getParent(e);
		gui->enableRect(parent, false);

		NetSystem& net = getNetSystem();
		m_to_server = net.connect("localhost", 12345);
		if (m_to_server == NetSystem::INVALID_CONNECTION) {
			logError("Failed to connect to server");
		}
		else {
			logInfo("Connected to server");
		}
	}

	void createServerClicked(EntityRef e) {
		GUIModule* gui = (GUIModule*)m_world.getModule("gui");
		EntityRef parent = *m_world.getParent(e);
		gui->enableRect(parent, false);

		NetSystem& net = getNetSystem();
		if (net.createServer(12345, 1)) {
			logInfo("Server creating at port 12345");
			m_is_server = true;
		}
		else {
			logError("Failed to create server");
		}
	}

	static void reflect() {
		LUMIX_MODULE(GameModule, "myplugin")
			.LUMIX_FUNC(connectClicked)
			.LUMIX_FUNC(createServerClicked)
		;
	}

	enum class Orientation { N, E, S, W };

	struct Player {
		Vec2 pos = {1, 1};
		EntityPtr entity;
		u32 free_bombs = 2;
		u32 flame_size = 4;
		float speed = 4;
		
		Orientation orientation = Orientation::N;
	};

	Tile m_board[15][11];
	Player m_player;

	bool m_left_input = false;
	bool m_right_input = false;
	bool m_up_input = false;
	bool m_down_input = false;
	bool m_vertical_prio = false;

	enum class GameState {
		NOT_RUNNING,
		LOADING,
		RUNNING
	};
	GameState m_game_state = GameState::NOT_RUNNING;

	Engine& m_engine;
	GameSystem& m_system;
	World& m_world;
	IAllocator& m_allocator;
	Array<EntityRef> m_explosions;
	PrefabResource* m_tile_prefabs[Tile::Type::COUNT] = {};
	PrefabResource* m_player_prefab = nullptr;
	PrefabResource* m_explosion_prefab = nullptr;

	NetSystem::ConnectionHandle m_to_server = NetSystem::INVALID_CONNECTION;
	NetSystem::ConnectionHandle m_to_client = NetSystem::INVALID_CONNECTION;
	bool m_is_server = false;
};


GameSystem::GameSystem(Engine& engine)
	: m_engine(engine)
{
	GameModule::reflect();
}


void GameSystem::createModules(World& world) {
	IAllocator& allocator = m_engine.getAllocator();
	UniquePtr<GameModule> module = UniquePtr<GameModule>::create(allocator, m_engine, *this, world, allocator);
	world.addModule(module.move());
}


LUMIX_PLUGIN_ENTRY(game)
{
	return LUMIX_NEW(engine.getAllocator(), GameSystem)(engine);
}


