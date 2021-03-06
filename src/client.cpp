#include <stdint.h>
#include <boost/asio.hpp>
#include <string>
#include <boost/shared_ptr.hpp>
#include <boost/bind.hpp>
#include <stdexcept>
#include <boost/foreach.hpp>
#include <boost/filesystem.hpp>

#include "hexradius.hpp"
#include "hexradius.pb.h"
#include "client.hpp"
#include "loadimage.hpp"
#include "fontstuff.hpp"
#include "powers.hpp"
#include "tile_anims.hpp"
#include "gui.hpp"
#include "menu.hpp"
#include "gamestate.hpp"
#include "tile.hpp"
#include "pawn.hpp"

const int EVENT_RDTIMER = 1;	// Redraw timer has fired
const int EVENT_RETURN = 2;	// Client should return - i.e. leave button pressed

static int within_rect(SDL_Rect rect, int x, int y) {
	return (x >= rect.x && x < rect.x+rect.w && y >= rect.y && y < rect.y+rect.h);
}

static void start_cb(const GUI::TextButton &, const SDL_Event &, Client *client) {
	client->send_begin();
}

static void push_sdl_event(int code) {
	SDL_Event l_event;

	l_event.type = SDL_USEREVENT;
	l_event.user.code = code;

	SDL_PushEvent(&l_event);
}

static void leave_cb(const GUI::TextButton &, const SDL_Event &) {
	push_sdl_event(EVENT_RETURN);
}

static Uint32 redraw_callback(Uint32 interval, void *) {
	push_sdl_event(EVENT_RDTIMER);
	return interval;
}

Client::Client(std::string host, uint16_t port) :
	quit(false), game_state(0),
	socket(io_service), redraw_timer(NULL), turn(0),
	state(CONNECTING), last_redraw(0), board(SDL_Rect()),
	dpawn(pawn_ptr()), mpawn(pawn_ptr()), hpawn(pawn_ptr()),
	pmenu_area(SDL_Rect()), lobby_gui(0, 0, 800, 600)
{
	lobby_gui.set_bg_image(ImgStuff::GetImage("graphics/menu/background.png"));

	boost::shared_ptr<GUI::TextButton> cm(new GUI::TextButton(lobby_gui, 300, 255, 200, 35, 0, "Connecting..."));
	lobby_buttons.push_back(cm);

	boost::shared_ptr<GUI::TextButton> ab(new GUI::TextButton(lobby_gui, 350, 310, 100, 35, 1, "Abort", &leave_cb));
	lobby_buttons.push_back(ab);

	boost::asio::ip::tcp::resolver resolver(io_service);
	boost::asio::ip::tcp::resolver::query query(host, "");
	boost::asio::ip::tcp::resolver::iterator it = resolver.resolve(query);

	boost::asio::ip::tcp::endpoint ep = *it;
	ep.port(port);

	socket.async_connect(ep, boost::bind(&Client::connect_callback, this, boost::asio::placeholders::error));

	redraw_timer = SDL_AddTimer(FRAME_DELAY, &redraw_callback, NULL);
	network_thread = boost::thread(boost::bind(&Client::net_thread_main, this));
}

Client::~Client() {
	if(redraw_timer) {
		SDL_RemoveTimer(redraw_timer);
	}

	io_service.stop();

	std::cout << "Waiting for client network thread to exit..." << std::endl;
	network_thread.join();

	delete game_state;

	for(anim_set::iterator anim = animators.begin(); anim != animators.end(); anim++) {
		delete *anim;
	}
}

void Client::net_thread_main() {
	try {
		io_service.run();
	} catch(std::runtime_error e) {
		protocol::message msg;
		msg.set_msg(protocol::QUIT);
		msg.set_quit_msg(std::string("Network error: ") + e.what());
		recv_queue.push(msg);
	}
}

void Client::connect_callback(const boost::system::error_code& error) {
	boost::unique_lock<boost::mutex> lock(the_mutex);

	if(error) {
		throw std::runtime_error("Connection failed: " + error.message());
	}

	protocol::message msg;
	msg.set_msg(protocol::INIT);
	msg.set_player_name(options.username);

	WriteProto(msg);

	ReadSize();
}

void Client::WriteProto(const protocol::message &msg) {
	send_queue.push(send_buf(msg));

	if(send_queue.size() == 1) {
		async_write(socket,
			    boost::asio::buffer(send_queue.front().buf.get(),
						send_queue.front().size),
			    boost::bind(&Client::WriteFinish,
					this,
					boost::asio::placeholders::error));
	}
}

void Client::WriteFinish(const boost::system::error_code& error) {
	boost::unique_lock<boost::mutex> lock(the_mutex);

	send_queue.pop();

	if(error) {
		throw std::runtime_error("Write error: " + error.message());
	}

	if(!send_queue.empty()) {
		async_write(socket,
			    boost::asio::buffer(send_queue.front().buf.get(),
						send_queue.front().size),
			    boost::bind(&Client::WriteFinish,
					this,
					boost::asio::placeholders::error));
	}
}

void Client::run() {
	SDL_Event event;

	while(SDL_WaitEvent(&event)) {
		boost::unique_lock<boost::mutex> lock(the_mutex);

		if(event.type == SDL_USEREVENT && event.user.code == EVENT_RETURN) {
			return;
		}

		if(event.type == SDL_QUIT) {
			quit = true;
			return;
		}

		while(!recv_queue.empty()) {
			protocol::message message = recv_queue.front();
			recv_queue.pop();

			handle_message(message);
		}
		if(dpawn && dpawn->destroyed()) dpawn.reset();
		if(mpawn && mpawn->destroyed()) mpawn.reset();
		if(hpawn && hpawn->destroyed()) hpawn.reset();
		if(direction_pawn && direction_pawn->destroyed()) direction_pawn.reset();
		if(target_pawn && target_pawn->destroyed()) target_pawn.reset();

		if(state == CONNECTING || state == LOBBY) {
			lobby_gui.handle_event(event);
			lobby_gui.redraw();

			continue;
		}

		if(event.type == SDL_MOUSEBUTTONDOWN && turn == my_id && tile_animators.empty()) {
			Tile *tile = game_state->tile_at_screen(event.button.x, event.button.y);

			if(event.button.button == SDL_BUTTON_LEFT) {
				xd = event.button.x;
				yd = event.button.y;

				if(tile && tile->pawn && tile->pawn->colour == my_colour && !target_pawn) {
					dpawn = tile->pawn;
				}
				else if(turn == my_id && xd > int(screen->w - RESIGN_BUTTON_WIDTH) && yd < int(RESIGN_BUTTON_HEIGHT)) {
					protocol::message msg;
					msg.set_msg(protocol::RESIGN);
					WriteProto(msg);
				}
			}
		}
		else if(event.type == SDL_MOUSEBUTTONUP && turn == my_id && tile_animators.empty()) {
			Tile *tile = game_state->tile_at_screen(event.button.x, event.button.y);

			pawn_ptr new_direction_pawn;
			pawn_ptr new_target_pawn;

			if(event.button.button == SDL_BUTTON_LEFT && xd == event.button.x && yd == event.button.y) {
				if(within_rect(pmenu_area, event.button.x, event.button.y)) {
					for(std::vector<pmenu_entry>::iterator i = pmenu.begin();
					    i != pmenu.end();
					    ++i) {
						if(!within_rect((*i).rect, event.button.x, event.button.y)) {
							continue;
						}
						unsigned int direction = Powers::powers[(*i).power].direction;
						if(direction == Powers::Power::targeted) {
							// Must pick a target, skip the direction menu.
							new_target_pawn = mpawn;
							direction_power = i->power;
						} else if((direction & (direction - 1)) == 0) {
							// This power uses exactly one direction and is not targeted
							// Don't draw the direction menu.
							protocol::message msg;
							msg.set_msg(protocol::USE);

							msg.add_pawns();
							mpawn->CopyToProto(msg.mutable_pawns(0), false);
							msg.mutable_pawns(0)->set_use_power((*i).power);
							msg.set_power_direction(direction);

							WriteProto(msg);
						} else {
							new_direction_pawn = mpawn;
							direction_power = i->power;
						}
					}

					dpawn.reset();
				} else if(within_rect(direction_menu_area, event.button.x, event.button.y)) {
					for(std::vector<pmenu_entry>::iterator i = direction_menu.begin();
					    i != direction_menu.end();
					    ++i) {
						if(!within_rect(i->rect, event.button.x, event.button.y)) {
							continue;
						}
						if((unsigned int)i->power == Powers::Power::targeted) {
							// Must pick a target
							new_target_pawn = mpawn;
							direction_power = i->power;
						} else {
							protocol::message msg;
							msg.set_msg(protocol::USE);

							msg.add_pawns();
							direction_pawn->CopyToProto(msg.mutable_pawns(0), false);
							msg.mutable_pawns(0)->set_use_power(direction_power);
							msg.set_power_direction(i->power);

							WriteProto(msg);
						}
						break;
					}
				} else if(target_pawn) {
					Tile *tile = game_state->tile_at_screen(event.button.x, event.button.y);
					if(tile && tile->pawn) {
						protocol::message msg;
						msg.set_msg(protocol::USE);

						msg.add_pawns();
						target_pawn->CopyToProto(msg.mutable_pawns(0), false);
						msg.mutable_pawns(0)->set_use_power(direction_power);
						msg.set_power_direction(Powers::Power::targeted);
						msg.add_tiles();
						msg.mutable_tiles(0)->set_col(tile->col);
						msg.mutable_tiles(0)->set_row(tile->row);

						WriteProto(msg);
					}
				}
			}

			direction_pawn = new_direction_pawn;
			target_pawn = new_target_pawn;
			mpawn.reset();

			if(event.button.button == SDL_BUTTON_LEFT && dpawn) {
				if(xd == event.button.x && yd == event.button.y) {
					if(tile->pawn->powers.size()) {
						mpawn = tile->pawn;
					}
				}
				else if(tile && tile != dpawn->cur_tile) {
					protocol::message msg;
					msg.set_msg(protocol::MOVE);
					msg.add_pawns();
					msg.mutable_pawns(0)->set_col(dpawn->cur_tile->col);
					msg.mutable_pawns(0)->set_row(dpawn->cur_tile->row);
					msg.mutable_pawns(0)->set_new_col(tile->col);
					msg.mutable_pawns(0)->set_new_row(tile->row);

					WriteProto(msg);
				}

				dpawn.reset();
			}
		}
		else if(event.type == SDL_MOUSEMOTION) {
			Tile *tile = game_state->tile_at_screen(event.motion.x, event.motion.y);

			if(dpawn) {
				last_redraw = 0;
			}
			else if(!mpawn && tile && tile->pawn) {
				if(hpawn != tile->pawn) {
					hpawn = tile->pawn;
					last_redraw = 0;
				}
			}
			else{
				hpawn.reset();
			}
		}
		else if(event.type == SDL_KEYDOWN) {
			if(event.key.keysym.scancode == 49) {
				int mouse_x, mouse_y;
				SDL_GetMouseState(&mouse_x, &mouse_y);

				Tile *tile = game_state->tile_at_screen(mouse_x, mouse_y);
				if(tile) {
					std::cout << "Mouse is over tile " << tile->col << "," << tile->row << std::endl;
				}else{
					std::cout << "The mouse isn't over a tile, you idiot." << std::endl;
				}
			}
		}

		if(SDL_GetTicks() >= last_redraw + FRAME_DELAY) {
			DrawScreen();
			last_redraw = SDL_GetTicks();
		}
	}
}

void Client::ReadSize(void) {
	async_read(socket,
		   boost::asio::buffer(&msgsize, sizeof(uint32_t)),
		   boost::bind(&Client::ReadMessage,
			       this,
			       boost::asio::placeholders::error));
}

void Client::ReadMessage(const boost::system::error_code& error) {
	boost::unique_lock<boost::mutex> lock(the_mutex);

	if(error) {
		throw std::runtime_error("Read error: " + error.message());
	}

	msgsize = ntohl(msgsize);

	if(msgsize > MAX_MSGSIZE) {
		throw std::runtime_error("Recieved oversized message from server");
	}

	msgbuf.resize(msgsize);
	async_read(socket,
		   boost::asio::buffer(&(msgbuf[0]), msgsize),
		   boost::bind(&Client::ReadFinish,
			       this,
			       boost::asio::placeholders::error));
}

void Client::ReadFinish(const boost::system::error_code& error) {
	boost::unique_lock<boost::mutex> lock(the_mutex);

	if(error) {
		throw std::runtime_error("Read error: " + error.message());
	}

	std::string msgstring(msgbuf.begin(), msgbuf.end());

	protocol::message msg;
	if(!msg.ParseFromString(msgstring)) {
		throw std::runtime_error("Invalid protobuf recieved from server");
	}

	recv_queue.push(msg);

	ReadSize();
}

void Client::handle_message(const protocol::message &msg) {
	if(msg.msg() == protocol::PQUIT) {
		if(msg.players_size() == 1) {
			PlayerColour c = (PlayerColour)msg.players(0).colour();

			std::cout << "Team " << c << " quit (" << msg.quit_msg() << ")" << std::endl;
			if(game_state) {
				game_state->destroy_team_pawns(c);
			}

			for(player_set::iterator p = players.begin(); p != players.end(); p++) {
				if((*p).colour == c) {
					players.erase(p);
					break;
				}
			}

			lobby_regen();
		}else{
			std::cerr << "PQUIT message recieved with " << msg.players_size() << " players, ignoring" << std::endl;
		}
	}else if(msg.msg() == protocol::QUIT) {
		std::cout << "You have been disconnected by the server (" << msg.quit_msg() << ")" << std::endl;
		push_sdl_event(EVENT_RETURN);
	}else{
		if(state == GAME) {
			handle_message_game(msg);
		}else{
			handle_message_lobby(msg);
		}
	}
}

void Client::handle_message_lobby(const protocol::message &msg) {
	if(msg.msg() == protocol::BEGIN) {
		game_state = new GameState;
		game_state->deserialize(msg);

		for(player_set::iterator p(players.begin()); p != players.end(); ++p) {
			Player *player = (Player *)&*p; // why is this const???
			player->score = 0;
		}

		state = GAME;

		TTF_Font *bfont = FontStuff::LoadFont("fonts/DejaVuSansMono-Bold.ttf", 14);
		int bskip = TTF_FontLineSkip(bfont);

		Tile::List::iterator tile = game_state->tiles.begin();

		board.x = 0;
		board.y = bskip;

		for(; tile != game_state->tiles.end(); tile++) {
			int w = 2*BOARD_OFFSET + (*tile)->col * TILE_WOFF + TILE_WIDTH + (((*tile)->row % 2) * TILE_ROFF);
			int h = 2*BOARD_OFFSET + (*tile)->row * TILE_HOFF + TILE_HEIGHT;

			if(board.w < w) {
				board.w = w;
			}
			if(board.h < h) {
				board.h = h;
			}
		}

		screen_w = board.w;
		screen_h = board.h+bskip;

		ImgStuff::set_mode(screen_w, screen_h);
	}else if(msg.msg() == protocol::GINFO) {
		state = LOBBY;

		my_id = msg.player_id();

		map_name = msg.map_name();

		for(int i = 0; i < msg.players_size(); i++) {
			Player p;
			p.name = msg.players(i).name();
			p.colour = (PlayerColour)msg.players(i).colour();
			p.id = msg.players(i).id();

			players.insert(p);

			if(p.id == my_id) {
				my_colour = p.colour;
			}
		}

		fog_of_war = msg.fog_of_war();
		if(msg.has_king_of_the_hill()) {
			king_of_the_hill = msg.king_of_the_hill();
		} else {
			king_of_the_hill = false;
		}

		lobby_regen();
	}else if(msg.msg() == protocol::PJOIN) {
		if(msg.players_size() == 1) {
			Player p;

			p.name = msg.players(0).name();
			p.colour = (PlayerColour)msg.players(0).colour();
			p.id = msg.players(0).id();

			players.insert(p);

			lobby_regen();
		}else{
			std::cerr << "PJOIN message recieved with " << msg.players_size() << " players, ignoring" << std::endl;
		}
	}else if(msg.msg() == protocol::CCOLOUR) {
		if(msg.players_size() == 1) {
			Player *p = get_player(msg.players(0).id());

			if(p) {
				p->colour = (PlayerColour)msg.players(0).colour();
				if(my_id == msg.players(0).id()) {
					my_colour = p->colour;
				}
				lobby_regen();
			}
		}else{
			std::cerr << "CCOLOUR message recieved with " << msg.players_size() << " players, ignoring" << std::endl;
		}
	} else if(msg.msg() == protocol::CHANGE_SETTING) {
		if(msg.has_fog_of_war()) {
			fog_of_war = msg.fog_of_war();
		}
		if(msg.has_king_of_the_hill()) {
			king_of_the_hill = msg.king_of_the_hill();
		}
		lobby_regen();
	}else{
		std::cerr << "Message " << msg.msg() << " recieved in LOBBY, ignoring" << std::endl;
	}
}

void Client::handle_message_game(const protocol::message &msg) {
	if(msg.msg() == protocol::TURN) {
		turn = msg.player_id();
		std::cout << "Turn for player " << turn << std::endl;
	}else if(msg.msg() == protocol::MOVE || msg.msg() == protocol::FORCE_MOVE) {
		if(msg.pawns_size() == 1) {
			pawn_ptr pawn = game_state->pawn_at(msg.pawns(0).col(), msg.pawns(0).row());
			Tile *tile = game_state->tile_at(msg.pawns(0).new_col(), msg.pawns(0).new_row());
			assert(tile);
			assert(pawn);
			assert(!tile->pawn);
			tile->pawn.swap(pawn->cur_tile->pawn);
			pawn->cur_tile = tile;
		}else{
			std::cerr << "Recieved MOVE message with " << msg.pawns_size() << " pawns, ignoring" << std::endl;
		}
	}else if(msg.msg() == protocol::DESTROY) {
		if(msg.pawns_size() == 1) {
			pawn_ptr pawn = game_state->pawn_at(msg.pawns(0).col(), msg.pawns(0).row());
			assert(pawn);
			pawn->destroy((Pawn::destroy_type)(-1));
		}else{
			std::cerr << "Recieved DESTROY message with " << msg.pawns_size() << " pawns, ignoring" << std::endl;
		}
	}else if(msg.msg() == protocol::UPDATE) {
		for(int i = 0; i < msg.tiles_size(); i++) {
			Tile *tile = game_state->tile_at(msg.tiles(i).col(), msg.tiles(i).row());
			if(!tile) {
				std::cerr << "Invalid tile " << msg.tiles(i).col() << "," << msg.tiles(i).col() << " update recieved from server! Out of sync?" << std::endl;
				continue;
			}

			tile->update_from_proto(msg.tiles(i));
		}

		for(int i = 0; i < msg.pawns_size(); i++) {
			pawn_ptr pawn = game_state->pawn_at(msg.pawns(i).col(), msg.pawns(i).row());
			if(!pawn) {
				std::cerr << "Invalid pawn " << msg.pawns(i).col() << "," << msg.pawns(i).col() << " update recieved from server! Out of sync?" << std::endl;
				continue;
			}

			pawn->flags = msg.pawns(i).flags();
			pawn->range = msg.pawns(i).range();
			pawn->colour = PlayerColour(msg.pawns(i).colour());
			std::map<int, int> old_powers(pawn->powers);
			pawn->powers.clear();

			for(int p = 0; p < msg.pawns(i).powers_size(); p++) {
				unsigned int index = msg.pawns(i).powers(p).index();
				int num = msg.pawns(i).powers(p).num();
				int old_num = old_powers[index];
				if (num < old_num) {
					for (int i = num; i < old_num; i++)
						pawn->cur_tile->power_messages.push_back(Tile::PowerMessage(index, false));
				}

				if(index >= Powers::powers.size() || num <= 0) {
					continue;
				}

				pawn->powers.insert(std::make_pair(index, num));
			}
		}
	}else if(msg.msg() == protocol::GOVER) {
		if(msg.is_draw()) {
			std::cout << "Game draw" << std::endl;
		}else{
			Player *winner = get_player(msg.player_id());
			assert(winner);

			std::cout << "Player '" << winner->name << "' won" << std::endl;
		}

		state = LOBBY;
		delete game_state;
		game_state = 0;

		ImgStuff::set_mode(MENU_WIDTH, MENU_HEIGHT);
	} else if(msg.msg() == protocol::PAWN_ANIMATION) {
		if(msg.animation_name() == "teleport") {
			if(msg.pawns_size() != 1) {
				std::cerr << "Recieved invalid teleport animation." << std::endl;
				return;
			}
			pawn_ptr pawn = game_state->pawn_at(msg.pawns(0).col(), msg.pawns(0).row());
			if(!pawn) {
				std::cerr << "Recieved invalid teleport animation. No such pawn " << msg.pawns(0).col() << "," << msg.pawns(0).col() << std::endl;
				return;
			}

			// Beware! The teleport animation message is sent before the move message.
			// This expects that the pawn will move soon after the animation starts playing.
			// The animation message contains source (col/row) tile and the target (new_col/new_row)
			// tile coordinates, but these aren't used yet.
			pawn->last_tile = pawn->cur_tile;
			pawn->last_tile->render_pawn = pawn;
			pawn->teleport_time = SDL_GetTicks();
		} else if(msg.animation_name() == "prod") {
			// Pawn 0 = originator
			// Pawn 1 = target
			if(msg.pawns_size() != 2) {
				std::cerr << "Recieved invalid teleport animation." << std::endl;
				return;
			}
			pawn_ptr pawn = game_state->pawn_at(msg.pawns(0).col(), msg.pawns(0).row());
			if(!pawn) {
				std::cerr << "Recieved invalid prod animation. No such pawn " << msg.pawns(0).col() << "," << msg.pawns(0).col() << std::endl;
				return;
			}
			pawn_ptr target = game_state->pawn_at(msg.pawns(1).col(), msg.pawns(1).row());
			if(!target) {
				std::cerr << "Recieved invalid prod animation. No such pawn " << msg.pawns(1).col() << "," << msg.pawns(1).col() << std::endl;
				return;
			}

			target->prod_time = SDL_GetTicks();
		} else {
			std::cerr << "Unknown pawn animation " << msg.animation_name() << std::endl;
		}
	} else if(msg.msg() == protocol::TILE_ANIMATION) {
		if(msg.animation_name() != "elevation") {
			std::cerr << "Received unsupported animation " << msg.animation_name() << std::endl;
			return;
		}
		bool got_delay_factor = false;
		bool got_mode = false;
		bool got_target_elevation = false;
		float delay_factor = 0.0f;
		TileAnimators::ElevationMode mode = TileAnimators::ABSOLUTE;
		int target_elevation = -1;
		for(int i = 0; i < msg.misc_size(); i++) {
			if(msg.misc(i).key() == "delay-factor") {
				got_delay_factor = true;
				delay_factor = msg.misc(i).float_value();
			} else if(msg.misc(i).key() == "mode") {
				got_mode = true;
				if(msg.misc(i).string_value() == "absolute") {
					mode = TileAnimators::ABSOLUTE;
				} else if(msg.misc(i).string_value() == "relative") {
					mode = TileAnimators::RELATIVE;
				} else {
					std::cerr << "Recieved unsupported elevation animation mode " << msg.misc(i).string_value() << std::endl;
					return;
				}
			} else if(msg.misc(i).key() == "target-elevation") {
				got_target_elevation = true;
				target_elevation = msg.misc(i).int_value();
			} else {
				std::cerr << "Recieved unsupported animation " << msg.animation_name() << std::endl;
				return;
			}
		}
		if(!got_target_elevation || !got_delay_factor || !got_mode || msg.tiles_size() < 1) {
			std::cerr << "Recieved unsupported animation " << msg.animation_name() << std::endl;
			return;
		}
		Tile *center = game_state->tile_at(msg.tiles(0).col(), msg.tiles(0).row());
		assert(center);
		Tile::List tiles;
		for(int i = 1; i < msg.tiles_size(); ++i) {
			Tile *tile = game_state->tile_at(msg.tiles(i).col(), msg.tiles(i).row());
			assert(tile);
			tiles.push_back(tile);
		}
		tile_animators.push_back(new TileAnimators::ElevationAnimator(tiles, center, delay_factor, mode, target_elevation));
	} else if(msg.msg() == protocol::PARTICLE_ANIMATION) {
		int tile_col = -1, tile_row = -1;
		for(int i = 0; i < msg.misc_size(); i++) {
			if(msg.misc(i).key() == "tile-col") {
				tile_col = msg.misc(i).int_value();
			} else if(msg.misc(i).key() == "tile-row") {
				tile_row = msg.misc(i).int_value();
			} else {
				std::cerr << "Recieved unsupported animation " << msg.animation_name() << std::endl;
				return;
			}
		}
		if(tile_col == -1 || tile_row == -1) {
			std::cerr << "Recieved unsupported animation " << msg.animation_name() << std::endl;
			return;
		}
		Tile *tile = game_state->tile_at(tile_col, tile_row);
		if(!tile) {
			std::cerr << "Recieved unsupported animation " << msg.animation_name() << std::endl;
			return;
		}
		if(msg.animation_name() == "crush") {
			add_animator(new Animators::PawnCrush(tile));
		} else if(msg.animation_name() == "pow") {
			add_animator(new Animators::PawnPow(tile));
		} else if(msg.animation_name() == "boom") {
			add_animator(new Animators::PawnBoom(tile));
		} else if(msg.animation_name() == "ohshitifelldownahole") {
			add_animator(new Animators::PawnOhShitIFellDownAHole(tile));
		} else {
			std::cerr << "Recieved unsupported animation " << msg.animation_name() << std::endl;
		}
	} else if(msg.msg() == protocol::ADD_POWER_NOTIFICATION) {
		assert(msg.pawns_size() == 1);
		Tile* tile = game_state->tile_at(msg.pawns(0).col(), msg.pawns(0).row());
		assert(tile);
		tile->power_messages.push_back(Tile::PowerMessage(msg.pawns(0).has_use_power() ?
								  msg.pawns(0).use_power() :
								  -1, true));
	} else if(msg.msg() == protocol::USE_POWER_NOTIFICATION) {
		assert(msg.pawns_size() == 1);
		Tile* tile = game_state->tile_at(msg.pawns(0).col(), msg.pawns(0).row());
		assert(tile);
		tile->power_messages.push_back(Tile::PowerMessage(msg.pawns(0).has_use_power() ?
								  msg.pawns(0).use_power() :
								  -1, false,
								  msg.has_power_direction() ?
								  msg.power_direction() : 0));
	} else if(msg.msg() == protocol::SCORE_UPDATE) {
		for(int i = 0; i < msg.players_size(); ++i) {
			Player *p = get_player(msg.players(i).id());
			p->score = msg.players(i).score();
		}
	}else{
		std::cerr << "Message " << msg.msg() << " recieved in GAME, ignoring" << std::endl;
	}
}

void Client::DrawScreen() {
	torus_frame = SDL_GetTicks() / 100 % (TORUS_FRAMES * 2);
	if (torus_frame >= TORUS_FRAMES)
		torus_frame = 2 * TORUS_FRAMES - torus_frame - 1;

	climb_offset = 2.5+(2.0*sin(SDL_GetTicks() / 300.0));

	SDL_Surface *tile = ImgStuff::GetImage("graphics/hextile.png");
	SDL_Surface *smashed_tile = ImgStuff::GetImage("graphics/hextile-broken.png");
	SDL_Surface *tint_tile = ImgStuff::GetImage("graphics/hextile.png", ImgStuff::TintValues(0,100,0));
	SDL_Surface *smashed_tint_tile = ImgStuff::GetImage("graphics/hextile-broken.png", ImgStuff::TintValues(0,100,0));
	SDL_Surface *jump_candidate_tile = ImgStuff::GetImage("graphics/hextile.png", ImgStuff::TintValues(0,0,100));
	SDL_Surface *smashed_jump_candidate_tile = ImgStuff::GetImage("graphics/hextile-broken.png", ImgStuff::TintValues(0,0,100));
	SDL_Surface *fow_tile = ImgStuff::GetImage("graphics/hextile.png", ImgStuff::TintValues(100,100,100));
	SDL_Surface *smashed_fow_tile = ImgStuff::GetImage("graphics/hextile-broken.png", ImgStuff::TintValues(100,100,100));
	SDL_Surface *hill_tile = ImgStuff::GetImage("graphics/hextile.png", ImgStuff::TintValues(212, 175, 55));
	SDL_Surface *smashed_hill_tile = ImgStuff::GetImage("graphics/hextile-broken.png", ImgStuff::TintValues(212, 175, 55));
	SDL_Surface *line_tile = ImgStuff::GetImage("graphics/hextile.png", ImgStuff::TintValues(0,20,0));
	SDL_Surface *smashed_line_tile = ImgStuff::GetImage("graphics/hextile-broken.png", ImgStuff::TintValues(0,20,0));
	SDL_Surface *target_tile = ImgStuff::GetImage("graphics/hextile.png", ImgStuff::TintValues(100,0,0));
	SDL_Surface *smashed_target_tile = ImgStuff::GetImage("graphics/hextile-broken.png", ImgStuff::TintValues(1000,0,0));
	SDL_Surface *pickup = ImgStuff::GetImage("graphics/pickup.png");
	SDL_Surface *mine = ImgStuff::GetImage("graphics/mines.png");
	SDL_Surface *landing_pad = ImgStuff::GetImage("graphics/landingpad.png");
	SDL_Surface *blackhole = ImgStuff::GetImage("graphics/blackhole.png");
	SDL_Surface *eye = ImgStuff::GetImage("graphics/eye.png");
	SDL_Surface *wrap = ImgStuff::GetImage("graphics/wrap.png");

	TTF_Font *font = FontStuff::LoadFont("fonts/DejaVuSansMono.ttf", 14);
	TTF_Font *bfont = FontStuff::LoadFont("fonts/DejaVuSansMono-Bold.ttf", 14);

	for (std::list<TileAnimators::Animator*>::iterator it = tile_animators.begin(); it != tile_animators.end(); it++) {
		if (!(*it)->do_stuff()) {
			delete *it;
			it = tile_animators.erase(it);
		}
	}

	ensure_SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 0, 0, 0));

	{
		SDL_Rect rect = {0,0,0,0};

		FontStuff::BlitText(screen, rect, font, ImgStuff::Colour(255,255,255), "Players: ");
		rect.x += FontStuff::TextWidth(font, "Players: ");

		for(player_set::iterator p = players.begin(); p != players.end(); p++) {
			if((*p).colour >= SPECTATE) {
				continue;
			}

			TTF_Font *f = (*p).id == turn ? bfont : font;

			std::vector<pawn_ptr> player_pawns = game_state->player_pawns((*p).colour);
			int visible_pawns = 0;
			int invisible_pawns = 0;
			for(std::vector<pawn_ptr>::iterator i = player_pawns.begin(); i != player_pawns.end(); ++i) {
				if((*i)->destroyed()) continue;
				if((*i)->flags & PWR_INVISIBLE) {
					invisible_pawns += 1;
				} else {
					visible_pawns += 1;
				}
			}

			std::string text = (*p).name + " (";
			if(visible_pawns == 0 && invisible_pawns == 0) {
				text += "defeated";
			} else if(my_colour == SPECTATE || my_colour == (*p).colour) {
				text += to_string(visible_pawns + invisible_pawns);
			} else {
				text += to_string(visible_pawns);
			}
			if(king_of_the_hill) {
				text += "  ";
				text += to_string(p->score);
			}
			text += ")  ";
			FontStuff::BlitText(screen, rect, f, team_colours[(*p).colour], text);
			rect.x += FontStuff::TextWidth(f, text);
		}

		if(turn == my_id) {
			rect.x = screen->w - RESIGN_BUTTON_WIDTH;
			rect.w = RESIGN_BUTTON_WIDTH;
			rect.h = RESIGN_BUTTON_HEIGHT;
			ensure_SDL_FillRect(screen, &rect, SDL_MapRGB(screen->format, 128, 0, 0));
			rect.x += (RESIGN_BUTTON_WIDTH - FontStuff::TextWidth(bfont, "Resign")) / 2;
			FontStuff::BlitText(screen, rect, bfont, ImgStuff::Colour(255, 255, 255), "Resign");
		}
	}

	Tile::List::iterator ti = game_state->tiles.begin();

	int mouse_x, mouse_y;
	SDL_GetMouseState(&mouse_x, &mouse_y);
	Tile *htile = game_state->tile_at_screen(mouse_x, mouse_y);

	int bs_col, fs_col, diag_row = -1;

	std::set<Tile *> infravision_tiles;
	bool spectate = my_colour == SPECTATE;
	if(!spectate) {
		std::vector<pawn_ptr> player_pawns = game_state->player_pawns(my_colour);
		spectate = true;
		for(std::vector<pawn_ptr>::iterator i = player_pawns.begin(); i != player_pawns.end(); ++i) {
			if(!(*i)->destroyed()) {
				spectate = false;
				break;
			}
		}
	}

	for(Tile::List::iterator ti = game_state->tiles.begin(); ti != game_state->tiles.end(); ++ti) {
		pawn_ptr p = (*ti)->pawn;
		if(spectate) {
			infravision_tiles.insert(*ti);
		}
		else if(p && p->colour == my_colour && (p->flags & PWR_INFRAVISION)) {
			Tile::List tiles;
			tiles = p->RadialTiles();
			infravision_tiles.insert(tiles.begin(), tiles.end());
			tiles = p->linear_tiles();
			infravision_tiles.insert(tiles.begin(), tiles.end());
		}
	}

	std::set<Tile *> visible_tiles;
	if(fog_of_war) {
		for(Tile::List::iterator ti = game_state->tiles.begin(); ti != game_state->tiles.end(); ++ti) {
			pawn_ptr p = (*ti)->pawn;
			if(p && p->colour == my_colour) {
				Tile::List tiles;
				tiles = p->RadialTiles(p->range+1);
				visible_tiles.insert(tiles.begin(), tiles.end());
				if(p->flags & PWR_INFRAVISION) {
					tiles = p->linear_tiles();
					visible_tiles.insert(tiles.begin(), tiles.end());
				}
			}
			if((*ti)->has_eye && (*ti)->eye_colour == my_colour) {
				Tile::List tiles = game_state->radial_tiles(*ti, 1);
				visible_tiles.insert(tiles.begin(), tiles.end());
			}
		}
	}

	Tile::List jump_tiles;
	if(hpawn && (hpawn->flags & PWR_JUMP)) {
		jump_tiles = hpawn->move_tiles();
	}

	for(int z = -2; z <= 2; z++) {
		for(Tile::List::iterator ti = game_state->tiles.begin(); ti != game_state->tiles.end(); ++ti) {
			if((*ti)->height != z) {
				continue;
			}
			if((*ti)->pawn) {
				assert(!(*ti)->pawn->destroyed());
			}
			SDL_Rect rect;
			rect.x = board.x + BOARD_OFFSET + TILE_WOFF * (*ti)->col + (((*ti)->row % 2) * TILE_ROFF);
			rect.y = board.y + BOARD_OFFSET + TILE_HOFF * (*ti)->row;
			rect.w = rect.h = 0;

			if ((*ti)->animating) {
				rect.x += (-1 * (*ti)->anim_height) * TILE_HEIGHT_FACTOR;
				rect.y += (-1 * (*ti)->anim_height) * TILE_HEIGHT_FACTOR;
			}
			else {
				rect.x += (-1 * (*ti)->height) * TILE_HEIGHT_FACTOR;
				rect.y += (-1 * (*ti)->height) * TILE_HEIGHT_FACTOR;
			}

			(*ti)->screen_x = rect.x;
			(*ti)->screen_y = rect.y;

			SDL_Surface *tile_img = (*ti)->smashed ? smashed_tile : tile;

			if(target_pawn && htile == *ti) {
				tile_img = (*ti)->smashed ? smashed_target_tile : target_tile;
			} else if(htile == *ti) {
				tile_img = (*ti)->smashed ? smashed_tint_tile : tint_tile;
			} else if(std::find(jump_tiles.begin(), jump_tiles.end(), *ti) != jump_tiles.end()) {
				tile_img = (*ti)->smashed ? smashed_jump_candidate_tile : jump_candidate_tile;
			} else if((fog_of_war && my_colour != SPECTATE) && visible_tiles.find(*ti) == visible_tiles.end()) {
				tile_img = (*ti)->smashed ? smashed_fow_tile : fow_tile;
			} else if(king_of_the_hill && (*ti)->hill) {
				tile_img = (*ti)->smashed ? smashed_hill_tile : hill_tile;
			} else if(htile && options.show_lines) {
				if(diag_row != (*ti)->row) {
					diag_cols(htile, (*ti)->row, bs_col, fs_col);
					diag_row = (*ti)->row;
				}

				if((*ti)->col == bs_col || (*ti)->col == fs_col || (*ti)->row == htile->row) {
					tile_img = (*ti)->smashed ? smashed_line_tile : line_tile;
				}
			}

			ensure_SDL_BlitSurface(tile_img, NULL, screen, &rect);

			if((*ti)->has_mine) {
				SDL_Rect s;
				s.x = 0;
				s.y = (*ti)->mine_colour * 50;
				s.w = s.h = 50;
				ensure_SDL_BlitSurface(mine, &s, screen, &rect);
			}
			if((*ti)->has_landing_pad) {
				SDL_Rect s;
				s.x = 0;
				s.y = (*ti)->landing_pad_colour * 50;
				s.w = s.h = 50;
				ensure_SDL_BlitSurface(landing_pad, &s, screen, &rect);
			}
			if((*ti)->has_black_hole) {
				ensure_SDL_BlitSurface(blackhole, NULL, screen, &rect);
			}

			if((*ti)->has_power) {
				ensure_SDL_BlitSurface(pickup, NULL, screen, &rect);
			}

			for (int wd = 0; wd < 6; wd++) {
				if ((*ti)->wrap & (1 << wd)) {
					SDL_Rect s;
					s.x = 0;
					s.y = wd * 50;
					s.w = s.h = 50;
					ensure_SDL_BlitSurface(wrap, &s, screen, &rect);
				}
			}
			
			if((*ti)->render_pawn && (*ti)->render_pawn != dpawn) {
				draw_pawn_tile((*ti)->render_pawn, *ti, infravision_tiles, visible_tiles);
			}
			else if((*ti)->pawn && (*ti)->pawn != dpawn) {
				draw_pawn_tile((*ti)->pawn, *ti, infravision_tiles, visible_tiles);
			}

			// Z-order. The eye is watching.
			if((*ti)->has_eye) {
				SDL_Rect s;
				s.x = 0;
				s.y = (*ti)->eye_colour * 50;
				s.w = s.h = 50;
				ensure_SDL_BlitSurface(eye, &s, screen, &rect);
			}
		}
	}

	for(anim_set::iterator ait = animators.begin(); ait != animators.end();) {
		anim_set::iterator current_anim = ait++;
		Animators::Generic *anim = *current_anim;

		if(!anim->render()) {
			delete anim;
			animators.erase(current_anim);
		}
	}

	if(dpawn) {
		SDL_Rect rect = {mouse_x-30, mouse_y-30, 0, 0}, base = {0,0,50,50};
		DrawPawn(dpawn, rect, base, std::set<Tile *>(), std::set<Tile *>());
	}

	std::vector<pawn_ptr> all_pawns = game_state->all_pawns();
	float dt = (SDL_GetTicks() - last_redraw) / 1000.0;
	for(Tile::List::iterator ti = game_state->tiles.begin(); ti != game_state->tiles.end(); ++ti) {
		for (std::list<Tile::PowerMessage>::iterator i = (*ti)->power_messages.begin(); i != (*ti)->power_messages.end(); ++i) {
			i->time -= dt;
			if (i->time > 0)
				draw_power_message(*ti, *i);
			else
				i = (*ti)->power_messages.erase(i);
		}
	}

	pmenu.clear();
	pmenu_area.w = 0;
	pmenu_area.h = 0;

	if(mpawn) {
		draw_pmenu(mpawn);
	}
	else if (!dpawn && hpawn && hpawn->colour == my_colour) {
		draw_pmenu(hpawn);
	}

	direction_menu.clear();
	direction_menu_area.w = 0;
	direction_menu_area.h = 0;
	if(direction_pawn) {
		draw_direction_menu(direction_pawn, direction_power);
	}

	SDL_UpdateRect(screen, 0, 0, 0, 0);
}

void Client::DrawPawn(pawn_ptr pawn, SDL_Rect rect, SDL_Rect base, const std::set<Tile *> &infravision_tiles, const std::set<Tile *> &visible_tiles) {
	bool invis = !!(pawn->flags & PWR_INVISIBLE);

	// Invisible pawns can't been seen by other players, unless exposed by infravision.
	if((invis && ((pawn->colour != my_colour) && my_colour != SPECTATE) &&
	     (infravision_tiles.find(pawn->cur_tile) == infravision_tiles.end())))
		return;
	// fog of war hides pawns.
	if((fog_of_war && visible_tiles.find(pawn->cur_tile) == visible_tiles.end()
		&& infravision_tiles.find(pawn->cur_tile) == infravision_tiles.end() && pawn != dpawn))
		return;

	const ImgStuff::TintValues tint(0, 0, 0, invis ? 128 : 255);
	SDL_Surface *pawn_graphics = ImgStuff::GetImage("graphics/pawns.png", tint);
	SDL_Surface *range_overlay = ImgStuff::GetImage("graphics/upgrades/range.png", tint);
	SDL_Surface *shadow = ImgStuff::GetImage("graphics/shadow.png", tint);
	SDL_Surface *shield = ImgStuff::GetImage("graphics/upgrades/shield.png", tint);
	SDL_Surface *infravision = ImgStuff::GetImage("graphics/upgrades/infravision.png", tint);
	SDL_Surface *bomb = ImgStuff::GetImage("graphics/upgrades/bomb.png", tint);
	SDL_Surface *confused = ImgStuff::GetImage("graphics/confused.png", tint);

	unsigned int frame = torus_frame;

	if (pawn != dpawn)
		ensure_SDL_BlitSurface(shadow, &base, screen, &rect);

	if(pawn->flags & PWR_CLIMB && pawn != dpawn) {
		rect.x -= climb_offset;
		rect.y -= climb_offset;
	}

	if(pawn == hpawn && pawn->colour == my_colour) {
		frame = 10;
	} else if(!pawn->has_power()) {
		frame = 0;
	}

	SDL_Rect srect = { frame * 50, (pawn->colour * 50) + base.y, 50, base.h };
	ensure_SDL_BlitSurface(pawn_graphics, &srect, screen, &rect);

	srect.x = pawn->range * 50;
	srect.y = (pawn->colour * 50) + base.y;
	ensure_SDL_BlitSurface(range_overlay, &srect, screen, &rect);

	if(pawn->flags & PWR_SHIELD)
		ensure_SDL_BlitSurface(shield, &base, screen, &rect);
	if(pawn->flags & PWR_INFRAVISION)
		ensure_SDL_BlitSurface(infravision, &base, screen, &rect);
	if(pawn->flags & PWR_CONFUSED)
		ensure_SDL_BlitSurface(confused, &base, screen, &rect);
	if(pawn->flags & PWR_BOMB)
		ensure_SDL_BlitSurface(bomb, &base, screen, &rect);
}

void Client::draw_pawn_tile(pawn_ptr pawn, Tile *tile, const std::set<Tile *> &infravision_tiles, const std::set<Tile *> &visible_tiles) {
	int teleport_y = 0;
	SDL_Rect rect = {tile->screen_x, tile->screen_y, 0, 0}, base = {0,0,50,50};

	if(pawn->last_tile) {
		if(pawn->teleport_time+1500 > SDL_GetTicks()) {
			teleport_y = (SDL_GetTicks() - pawn->teleport_time) / 30;

			if(pawn->last_tile == tile) {
				rect.y += teleport_y;

				base.y += teleport_y;
				base.h -= teleport_y;
			}else{
				base.h = teleport_y;
			}
		}else{
			pawn->last_tile->render_pawn.reset();
			pawn->last_tile = NULL;
		}
	}
	if(pawn->prod_time) {
		float prod = (SDL_GetTicks() - pawn->prod_time) / 500.0f;
		if(prod < 1.0) {
			rect.y -= 2.0f*sinf(prod * M_PI * 2.0f);
		} else {
			pawn->prod_time = 0;
		}
	}

	if(pawn->cur_tile != tile && pawn->last_tile != tile) {
		return;
	}

	DrawPawn(pawn, rect, base, infravision_tiles, visible_tiles);
}

void Client::lobby_regen() {
	int y = 65;

	lobby_buttons.clear();
	lobby_players.clear();
	map_chooser.clear();
	colour_choosers.clear();
	lobby_settings.clear();

	boost::shared_ptr<GUI::TextButton> pn(new GUI::TextButton(lobby_gui, 20, 20, 300, 35, 0, "Player Name"));
	pn->align(GUI::LEFT);
	lobby_buttons.push_back(pn);

	boost::shared_ptr<GUI::TextButton> pc(new GUI::TextButton(lobby_gui, 330, 20, 135, 35, 0, "Team"));
	pc->align(GUI::LEFT);
	lobby_buttons.push_back(pc);

	if(my_id == ADMIN_ID) {
		boost::shared_ptr<GUI::TextButton> ai(new GUI::TextButton(lobby_gui, 535, 130, 135, 35, 5, "Add AI",
									  boost::bind(&Client::add_ai, this, _1, _2)));
		lobby_buttons.push_back(ai);
	}

	boost::shared_ptr<GUI::Checkbox> fow(new GUI::Checkbox(lobby_gui, 535, 65, 25, 25, 0, fog_of_war, my_id == ADMIN_ID));
	fow->set_callback(boost::bind(&Client::fog_of_war_cb, this, _1));
	lobby_settings.push_back(fow);

	boost::shared_ptr<GUI::TextButton> fow_label(new GUI::TextButton(lobby_gui, 535+30, 65, 160, 25, 0, "Fog of War"));
	fow_label->align(GUI::LEFT);
	lobby_buttons.push_back(fow_label);

	boost::shared_ptr<GUI::Checkbox> koth(new GUI::Checkbox(lobby_gui, 535, 95, 25, 25, 0, king_of_the_hill, my_id == ADMIN_ID));
	koth->set_callback(boost::bind(&Client::king_of_the_hill_cb, this, _1));
	lobby_settings.push_back(koth);

	boost::shared_ptr<GUI::TextButton> koth_label(new GUI::TextButton(lobby_gui, 535+30, 95, 160, 25, 0, "King of the Hill"));
	koth_label->align(GUI::LEFT);
	lobby_buttons.push_back(koth_label);

	if(my_id == ADMIN_ID) {
		boost::shared_ptr< GUI::DropDown<std::string> > mn(new GUI::DropDown<std::string>(lobby_gui, 475, 20, 305, 35, 1));
		map_chooser.push_back(mn);

		mn->callback = boost::bind(&Client::change_map, this, _1);

		using namespace boost::filesystem;

		for(directory_iterator node("scenario"); node != directory_iterator(); ++node)
		{
			std::string name = node->path().string().substr(9);
			mn->add_item(name, name);
		}

		mn->select(map_name);

		boost::shared_ptr<GUI::TextButton> sg(new GUI::TextButton(lobby_gui, 645, 339, 135, 35, 2, "Start Game", boost::bind(start_cb, _1, _2, this)));
		lobby_buttons.push_back(sg);
	}
	else{
		boost::shared_ptr<GUI::TextButton> mn(new GUI::TextButton(lobby_gui, 475, 20, 305, 35, 0, map_name));
		lobby_buttons.push_back(mn);
	}

	boost::shared_ptr<GUI::TextButton> lg(new GUI::TextButton(lobby_gui, 645, 384, 135, 35, 3, "Leave Game", leave_cb));
	lobby_buttons.push_back(lg);

	for(player_set::iterator p = players.begin(); p != players.end(); p++) {
		boost::shared_ptr<GUI::TextButton> pn(new GUI::TextButton(lobby_gui, 20, y, 300, 35, 0, (*p).name));
		pn->align(GUI::LEFT);
		lobby_players.push_back(pn);

		if(my_id == p->id || my_id == ADMIN_ID) {
			boost::shared_ptr< GUI::DropDown<PlayerColour> > pc(new GUI::DropDown<PlayerColour>(lobby_gui, 330, y, 135, 35, y));

			for(int i = 0; i < 7; i++)
			{
				pc->add_item((PlayerColour)(i), team_names[i], team_colours[i]);
			}

			pc->select(p->colour);

			pc->callback = boost::bind(&Client::change_colour, this, p->id, _1);

			colour_choosers.push_back(pc);
		}else{
			boost::shared_ptr<GUI::TextButton> pc(new GUI::TextButton(lobby_gui, 330, y, 135, 35, 0, team_names[p->colour]));
			pc->align(GUI::LEFT);
			pc->set_fg_colour(team_colours[p->colour]);

			lobby_players.push_back(pc);
		}

		if(my_id == ADMIN_ID && p->id != ADMIN_ID) {
			boost::shared_ptr<GUI::TextButton> pkick(new GUI::TextButton(lobby_gui, 475, y, 50, 35, 0, "Kick",
										     boost::bind(&Client::kick, this, p->id, _1, _2)));
			pkick->enabled = true;
			lobby_buttons.push_back(pkick);
		}

		y += 40;
	}
}

void Client::fog_of_war_cb(const GUI::Checkbox &checkbox)
{
	protocol::message msg;
	msg.set_msg(protocol::CHANGE_SETTING);
	msg.set_fog_of_war(checkbox.state);
	WriteProto(msg);
}

void Client::king_of_the_hill_cb(const GUI::Checkbox &checkbox)
{
	protocol::message msg;
	msg.set_msg(protocol::CHANGE_SETTING);
	msg.set_king_of_the_hill(checkbox.state);
	WriteProto(msg);
}

void Client::send_begin() {
	protocol::message msg;
	msg.set_msg(protocol::BEGIN);

	WriteProto(msg);
}

bool Client::change_colour(uint16_t id, PlayerColour colour)
{
	protocol::message msg;
	msg.set_msg(protocol::CCOLOUR);

	msg.add_players();
	msg.mutable_players(0)->set_id(id);
	msg.mutable_players(0)->set_colour((protocol::colour)colour);

	WriteProto(msg);

	return false;
}

bool Client::kick(uint16_t id, const GUI::TextButton &, const SDL_Event &)
{
	std::cout << "Kick " << id << std::endl;
	protocol::message msg;
	msg.set_msg(protocol::KICK);
	msg.set_player_id(id);

	WriteProto(msg);

	return false;
}

bool Client::add_ai(const GUI::TextButton &, const SDL_Event &)
{
	protocol::message msg;
	msg.set_msg(protocol::ADD_AI);
	WriteProto(msg);

	return false;
}

bool Client::change_map(const std::string &map)
{
	protocol::message msg;
	msg.set_msg(protocol::CHANGE_MAP);
	msg.set_map_name(map);

	WriteProto(msg);

	return false;
}

void Client::add_animator(Animators::Generic* anim) {
	animators.insert(anim);
}

void Client::diag_cols(Tile *htile, int row, int &bs_col, int &fs_col) {
	bs_col = htile->col;
	fs_col = htile->col;

	if(htile->row == row) {
		return;
	}

	for(int r = htile->row-1; r >= row; r--) {
		if(r % 2) {
			bs_col--;
		}else{
			fs_col++;
		}

		if(r == row) {
			return;
		}
	}

	bs_col = htile->col;
	fs_col = htile->col;

	for(int r = htile->row+1; r <= row; r++) {
		if(r % 2) {
			fs_col--;
		}else{
			bs_col++;
		}

		if(r == row) {
			return;
		}
	}
}

// These are appened to power names based on the directionality of the power.
// They must to be seperate from the actual name because they have to be
// rendered using DejaVu Serif.
// Save this file with UTF-8 or I'll set you on fire.
static struct {
	unsigned int direction;
	const char *string;
} direction_entry[] = {
	{Powers::Power::radial,              "⥁"}, // U+2941 CLOCKWISE CLOSED CIRCLE ARROW
	//{Powers::Power::radial,              "⎔"}, // U+2394 SOFTWARE-FUNCTION SYMBOL (not supported by dejavuserif)
	{Powers::Power::east_west,           "↔"}, // U+2194 LEFT RIGHT ARROW
	{Powers::Power::northeast_southwest, "⤢"}, // U+2922 NORTH EAST AND SOUTH WEST ARROW
	{Powers::Power::northwest_southeast, "⤡"}, // U+2921 NORTH WEST AND SOUTH EAST ARROW
	{Powers::Power::east,                "→"}, // U+2192 RIGHTWARDS ARROW
	{Powers::Power::southeast,           "↘"}, // U+2198 SOUTH EAST ARROW
	{Powers::Power::southwest,           "↙"}, // U+2199 SOUTH WEST ARROW
	{Powers::Power::west,                "←"}, // U+2190 LEFTWARDS ARROW
	{Powers::Power::northwest,           "↖"}, // U+2196 NORTH WEST ARROW
	{Powers::Power::northeast,           "↗"}, // U+2197 NORTH EAST ARROW
	//{Powers::Power::targeted,            "⦻"}, // U+29BB CIRCLE WITH SUPERIMPOSED X (not supported)
	{Powers::Power::targeted,            "¤"}, // U+00A4 CURRENCY SIGN
	//{Powers::Power::point,               "・"}, // U+30FB KATAKANA MIDDLE DOT (not supported)
	{Powers::Power::point,               "•"}, // U+2022 BULLET
};

static std::string direction_symbol(unsigned int direction)
{
	std::string result;

	for(unsigned int i = 0; i < sizeof direction_entry / sizeof direction_entry[0]; ++i) {
		if(direction & direction_entry[i].direction) {
			result += direction_entry[i].string;
		}
	}

	return result;
}

void Client::draw_pmenu(pawn_ptr pawn) {
	TTF_Font *font = FontStuff::LoadFont("fonts/DejaVuSansMono.ttf", 14);
	TTF_Font *symbol_font = FontStuff::LoadFont("fonts/DejaVuSerif.ttf", 14);

	int fh = std::max(TTF_FontLineSkip(font), TTF_FontLineSkip(symbol_font));
	int fw = FontStuff::TextWidth(font, "0");

	int mouse_x, mouse_y;
	SDL_GetMouseState(&mouse_x, &mouse_y);

	SDL_Rect rect = {
		pawn->cur_tile->screen_x+TILE_WIDTH,
		pawn->cur_tile->screen_y,
		0,
		pawn->powers.size() * fh + ((pawn->flags & PWR_JUMP) ? fh+1 : 0)
	};

	if(pawn->flags & PWR_JUMP) {
		int w = FontStuff::TextWidth(font, "Jump");
		if(w > rect.w) {
			rect.w = w;
		}
	}

	for(Pawn::PowerList::iterator i = pawn->powers.begin(); i != pawn->powers.end(); i++) {
		Powers::Power &power = Powers::powers[i->first];
		int w = FontStuff::TextWidth(font, power.name);
		if(power.direction != Powers::Power::undirected) {
			std::string direction = direction_symbol(power.direction);
			w += FontStuff::TextWidth(symbol_font, " ");
			w += FontStuff::TextWidth(symbol_font, direction);
		}
		if(w > rect.w) {
			rect.w = w;
		}
	}

	if(!pawn->powers.empty()) {
		rect.w += fw*3;
	}

	if(rect.x+rect.w > screen_w) {
		rect.x = pawn->cur_tile->screen_x-rect.w;
	}
	if(rect.y+rect.h > screen_h) {
		rect.y = pawn->cur_tile->screen_y-rect.h;
	}

	ImgStuff::draw_rect(rect, ImgStuff::Colour(0,0,0), 178);

	pmenu_area = rect;
	rect.h = fh;

	SDL_Color font_colour = {0,255,0, 0};

	if(pawn->flags & PWR_JUMP) {
		// ?????
		if(mouse_x >= rect.x && mouse_x < rect.x+rect.w && mouse_y >= rect.y && mouse_y < rect.y+rect.h) {
			ImgStuff::draw_rect(rect, ImgStuff::Colour(90,90,0), 178);
		}
		// Have this centered?
		FontStuff::BlitText(screen, rect, font, font_colour, "Jump");
		rect.y += fh+1;
		// Would like to draw a line seperating transient upgrades from powers, but whatever.
	}

	for(Pawn::PowerList::iterator i = pawn->powers.begin(); i != pawn->powers.end(); i++) {
		pmenu_entry foobar = {rect, i->first};
		pmenu.push_back(foobar);

		if(mouse_x >= rect.x && mouse_x < rect.x+rect.w && mouse_y >= rect.y && mouse_y < rect.y+rect.h) {
			ImgStuff::draw_rect(rect, ImgStuff::Colour(90,90,0), 178);
		}

		FontStuff::BlitText(screen, rect, font, font_colour, to_string(i->second));

		int original_x = rect.x;
		rect.x += fw*3;

		Powers::Power &power = Powers::powers[i->first];
		rect.x += FontStuff::BlitText(screen, rect, font, font_colour, power.name);
		if(power.direction != Powers::Power::undirected) {
			std::string direction = direction_symbol(power.direction);
			rect.x += FontStuff::BlitText(screen, rect, symbol_font, font_colour, " ");
			rect.x += FontStuff::BlitText(screen, rect, symbol_font, font_colour, direction);
		}

		rect.x = original_x;
		rect.y += fh;
	}
}

void Client::draw_power_message(Tile* tile, Tile::PowerMessage& pm) {
	TTF_Font *font = FontStuff::LoadFont("fonts/DejaVuSansMono.ttf", 14);
	TTF_Font *symbol_font = FontStuff::LoadFont("fonts/DejaVuSerif.ttf", 14);

	bool hide = pm.power == -1;

	std::string str = hide ? "???" : Powers::powers[pm.power].name;
	std::string direction_text;
	str = (pm.added ? "+ " : "- ") + str;

	if(!hide && pm.direction) {
		direction_text += " ";
		direction_text += direction_symbol(pm.direction);
	}

	int fh = std::max(TTF_FontLineSkip(font), TTF_FontLineSkip(symbol_font));
	int fw = FontStuff::TextWidth(font, "0");

	SDL_Rect rect;
	rect.w = FontStuff::TextWidth(font, str);
	rect.w += FontStuff::TextWidth(symbol_font, direction_text);
	rect.w += fw;
	rect.h = fh;
	rect.x = tile->screen_x - rect.w / 2 + TILE_WIDTH / 2;
	rect.y = tile->screen_y - 32 + 16 * pm.time;

	ImgStuff::draw_rect(rect, ImgStuff::Colour(0,0,0), 178 * std::min(pm.time, 1.0f));

	SDL_Color font_colour = {0, 255, 0, 0};

	rect.x += FontStuff::BlitText(screen, rect, font, font_colour, str);
	rect.x += FontStuff::BlitText(screen, rect, symbol_font, font_colour, direction_text);
}

void Client::draw_direction_menu(pawn_ptr pawn, int power_id)
{
	Powers::Power &power = Powers::powers[power_id];
	TTF_Font *symbol_font = FontStuff::LoadFont("fonts/DejaVuSerif.ttf", 24);

	int fh = TTF_FontLineSkip(symbol_font);

	int mouse_x, mouse_y;
	SDL_GetMouseState(&mouse_x, &mouse_y);

	SDL_Rect rect = {
		pawn->cur_tile->screen_x+TILE_WIDTH,
		pawn->cur_tile->screen_y,
		0, fh
	};

	for(unsigned int i = 0; i < sizeof direction_entry / sizeof direction_entry[0]; ++i) {
		if(power.direction & direction_entry[i].direction) {
			rect.w += FontStuff::TextWidth(symbol_font, direction_entry[i].string);
		}
	}

	if(rect.x+rect.w > screen_w) {
		rect.x = pawn->cur_tile->screen_x-rect.w;
	}
	if(rect.y+rect.h > screen_h) {
		rect.y = pawn->cur_tile->screen_y-rect.h;
	}

	ImgStuff::draw_rect(rect, ImgStuff::Colour(0,0,0), 178);

	direction_menu_area = rect;
	rect.h = fh;

	SDL_Color font_colour = {0,255,0, 0};

	for(unsigned int i = 0; i < sizeof direction_entry / sizeof direction_entry[0]; ++i) {
		if((power.direction & direction_entry[i].direction) == 0) {
			continue;
		}
		int width = FontStuff::TextWidth(symbol_font, direction_entry[i].string);

		if(mouse_x >= rect.x && mouse_x < rect.x+width && mouse_y >= rect.y && mouse_y < rect.y+rect.h) {
			int tmp = rect.w;
			rect.w = width;
			pmenu_entry foobar = {rect, direction_entry[i].direction};
			direction_menu.push_back(foobar);
			ImgStuff::draw_rect(rect, ImgStuff::Colour(90,90,0), 178);
			rect.w = tmp;
		}

		FontStuff::BlitText(screen, rect, symbol_font, font_colour, direction_entry[i].string);

		rect.x += width;
	}
}
