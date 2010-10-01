#include <stdint.h>
#include <boost/asio.hpp>
#include <string>
#include <boost/shared_ptr.hpp>
#include <boost/bind.hpp>
#include <stdexcept>

#include "octradius.hpp"
#include "octradius.pb.h"
#include "client.hpp"
#include "loadimage.hpp"
#include "fontstuff.hpp"
#include "powers.hpp"
#include "tile_anims.hpp"
#include "gui.hpp"
#include "menu.hpp"

const int EVENT_RDTIMER = 1;	// Redraw timer has fired
const int EVENT_RETURN = 2;	// Client should return - i.e. leave button pressed

static int within_rect(SDL_Rect rect, int x, int y) {
	return (x >= rect.x && x < rect.x+rect.w && y >= rect.y && y < rect.y+rect.h);
}

static void start_cb(const GUI::TextButton &button, const SDL_Event &event, void *arg) {
	Client *client = (Client*)arg;
	client->send_begin();
}

static void leave_cb(const GUI::TextButton &btn, const SDL_Event &event, void *arg) {
	SDL_Event l_event;
	
	l_event.type = SDL_USEREVENT;
	l_event.user.code = EVENT_RETURN;
	
	SDL_PushEvent(&l_event);
}

static Uint32 redraw_callback(Uint32 interval, void *param) {
	SDL_Event event;
	
	event.type = SDL_USEREVENT;
	event.user.code = EVENT_RDTIMER;
	
	SDL_PushEvent(&event);
	
	return interval;
}

Client::Client(std::string host, uint16_t port) : quit(false), socket(io_service), redraw_timer(NULL), turn(0), state(CONNECTING), last_redraw(0), board(SDL_Rect()), dpawn(NULL), mpawn(NULL), hpawn(NULL), pmenu_area(SDL_Rect()), current_animator(NULL), lobby_gui(0, 0, 800, 600) {
	lobby_gui.set_bg_image(ImgStuff::GetImage("graphics/menu/background.png"));
	
	boost::shared_ptr<GUI::TextButton> cm(new GUI::TextButton(lobby_gui, 300, 255, 200, 35, 0, "Connecting..."));
	lobby_buttons.push_back(cm);
	
	boost::shared_ptr<GUI::TextButton> ab(new GUI::TextButton(lobby_gui, 350, 310, 100, 35, 1, "Abort", &leave_cb, this));
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
	
	FreeTiles(tiles);
	
	for(anim_set::iterator anim = animators.begin(); anim != animators.end(); anim++) {
		(*anim)->free();
	}
}

void Client::net_thread_main() {
	io_service.run();
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
		async_write(socket, boost::asio::buffer(send_queue.front().buf.get(), send_queue.front().size), boost::bind(&Client::WriteFinish, this, boost::asio::placeholders::error));
	}
}

void Client::WriteFinish(const boost::system::error_code& error) {
	boost::unique_lock<boost::mutex> lock(the_mutex);
	
	send_queue.pop();
	
	if(error) {
		throw std::runtime_error("Write error: " + error.message());
	}
	
	if(!send_queue.empty()) {
		async_write(socket, boost::asio::buffer(send_queue.front().buf.get(), send_queue.front().size), boost::bind(&Client::WriteFinish, this, boost::asio::placeholders::error));
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
		
		if(state == CONNECTING || state == LOBBY) {
			lobby_gui.handle_event(event);
			lobby_gui.poll(false);
			
			continue;
		}
		
		if(event.type == SDL_MOUSEBUTTONDOWN && turn == my_id && !current_animator) {
			Tile *tile = TileAtXY(tiles, event.button.x, event.button.y);
			
			if(event.button.button == SDL_BUTTON_LEFT) {
				xd = event.button.x;
				yd = event.button.y;
				
				if(tile && tile->pawn && tile->pawn->colour == my_colour) {
					dpawn = tile->pawn;
				}
			}
		}
		else if(event.type == SDL_MOUSEBUTTONUP && turn == my_id && !current_animator) {
			Tile *tile = TileAtXY(tiles, event.button.x, event.button.y);
			
			if(event.button.button == SDL_BUTTON_LEFT && xd == event.button.x && yd == event.button.y) {
				if(within_rect(pmenu_area, event.button.x, event.button.y)) {
					std::vector<pmenu_entry>::iterator i = pmenu.begin();
					
					while(i != pmenu.end()) {
						if(within_rect((*i).rect, event.button.x, event.button.y)) {
							protocol::message msg;
							msg.set_msg(protocol::USE);
							
							msg.add_pawns();
							mpawn->CopyToProto(msg.mutable_pawns(0), false);
							msg.mutable_pawns(0)->set_use_power((*i).power);
							
							WriteProto(msg);
							break;
						}
						
						i++;
					}
					
					dpawn = NULL;
				}
			}
			
			mpawn = NULL;
			
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
				
				dpawn = NULL;
			}
		}
		else if(event.type == SDL_MOUSEMOTION) {
			Tile *tile = TileAtXY(tiles, event.motion.x, event.motion.y);
			
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
				hpawn = NULL;
			}
		}
		else if(event.type == SDL_KEYDOWN) {
			if(event.key.keysym.scancode == 49) {
				int mouse_x, mouse_y;
				SDL_GetMouseState(&mouse_x, &mouse_y);
				
				Tile *tile = TileAtXY(tiles, mouse_x, mouse_y);
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
	async_read(socket, boost::asio::buffer(&msgsize, sizeof(uint32_t)), boost::bind(&Client::ReadMessage, this, boost::asio::placeholders::error));
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
	async_read(socket, boost::asio::buffer(&(msgbuf[0]), msgsize), boost::bind(&Client::ReadFinish, this, boost::asio::placeholders::error));
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
			DestroyTeamPawns(tiles, c);
			
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
		abort();
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
		for(int i = 0; i < msg.tiles_size(); i++) {
			tiles.push_back(new Tile(msg.tiles(i).col(), msg.tiles(i).row(), msg.tiles(i).height()));
		}
		
		for(int i = 0; i < msg.pawns_size(); i++) {
			Tile *tile = FindTile(tiles, msg.pawns(i).col(), msg.pawns(i).row());
			if(!tile) {
				continue;
			}
			
			tile->pawn = new Pawn((PlayerColour)msg.pawns(i).colour(), tiles, tile);
		}
		
		state = GAME;
		
		TTF_Font *bfont = FontStuff::LoadFont("fonts/DejaVuSansMono-Bold.ttf", 14);
		int bskip = TTF_FontLineSkip(bfont);
		
		Tile::List::iterator tile = tiles.begin();
		
		board.x = 0;
		board.y = bskip;
		
		for(; tile != tiles.end(); tile++) {
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
		
		screen = SDL_SetVideoMode(screen_w, screen_h, 0, SDL_SWSURFACE);
		assert(screen != NULL);
	}else if(msg.msg() == protocol::GINFO) {
		state = LOBBY;
		
		my_id = msg.player_id();
		
		std::cout << "My ID = " << my_id << std::endl;
		
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
		
		lobby_buttons.clear();
		
		boost::shared_ptr<GUI::TextButton> pn(new GUI::TextButton(lobby_gui, 20, 20, 300, 35, 0, "Player Name"));
		pn->align(GUI::LEFT);
		lobby_buttons.push_back(pn);
		
		boost::shared_ptr<GUI::TextButton> pc(new GUI::TextButton(lobby_gui, 330, 20, 135, 35, 0, "Team"));
		pc->align(GUI::LEFT);
		lobby_buttons.push_back(pc);
		
		if(my_id == ADMIN_ID) {
			boost::shared_ptr<GUI::TextButton> sg(new GUI::TextButton(lobby_gui, 645, 339, 135, 35, 1, "Start Game", &start_cb, this));
			lobby_buttons.push_back(sg);
		}
		
		boost::shared_ptr<GUI::TextButton> lg(new GUI::TextButton(lobby_gui, 645, 384, 135, 35, 2, "Leave Game", &leave_cb, this));
		lobby_buttons.push_back(lg);
		
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
				lobby_regen();
			}
		}else{
			std::cerr << "CCOLOUR message recieved with " << msg.players_size() << " players, ignoring" << std::endl;
		}
	}else{
		std::cerr << "Message " << msg.msg() << " recieved in LOBBY, ignoring" << std::endl;
	}
}

void Client::handle_message_game(const protocol::message &msg) {
	if(msg.msg() == protocol::TURN) {
		turn = msg.player_id();
		std::cout << "Turn for player " << turn << std::endl;
	}else if(msg.msg() == protocol::MOVE) {
		if(msg.pawns_size() == 1) {
			Pawn *pawn = FindPawn(tiles, msg.pawns(0).col(), msg.pawns(0).row());
			Tile *tile = FindTile(tiles, msg.pawns(0).new_col(), msg.pawns(0).new_row());
			
			if(!(pawn && tile && pawn->Move(tile, NULL, this))) {
				std::cerr << "Invalid move recieved from server! Out of sync?" << std::endl;
			}
		}else{
			std::cerr << "Recieved MOVE message with " << msg.pawns_size() << " pawns, ignoring" << std::endl;
		}
	}else if(msg.msg() == protocol::UPDATE) {
		for(int i = 0; i < msg.tiles_size(); i++) {
			Tile *tile = FindTile(tiles, msg.tiles(i).col(), msg.tiles(i).row());
			if(!tile) {
				continue;
			}
			
			tile->height = msg.tiles(i).height();
			tile->has_power = msg.tiles(i).power();
		}
		
		for(int i = 0; i < msg.pawns_size(); i++) {
			Pawn *pawn = FindPawn(tiles, msg.pawns(i).col(), msg.pawns(i).row());
			if(!pawn) {
				continue;
			}
			
			pawn->flags = msg.pawns(i).flags();
			pawn->powers.clear();
			
			for(int p = 0; p < msg.pawns(i).powers_size(); p++) {
				int index = msg.pawns(i).powers(p).index();
				int num = msg.pawns(i).powers(p).num();
				
				if(index < 0 || index >= Powers::num_powers || num <= 0) {
					continue;
				}
				
				pawn->powers.insert(std::make_pair(index, num));
			}
		}
	}else if(msg.msg() == protocol::USE) {
		if(msg.pawns_size() == 1) {
			Pawn *pawn = FindPawn(tiles, msg.pawns(0).col(), msg.pawns(0).row());
			
			if(pawn) {
				int power = msg.pawns(0).use_power();
				
				power_rand_vals.clear();
				
				for(int i = 0; i < msg.power_rand_vals_size(); i++) {
					power_rand_vals.push_back(msg.power_rand_vals(i));
				}
				
				if(pawn->powers.size()) {
					pawn->UsePower(power, NULL, this);
				}else if(power >= 0 && power < Powers::num_powers) {
					Powers::powers[power].func(pawn, NULL, this);
				}
			}
		}else{
			std::cerr << "Recieved USE message with " << msg.pawns_size() << " pawns, ignoring" << std::endl;
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
		FreeTiles(tiles);
		
		screen = SDL_SetVideoMode(MENU_WIDTH, MENU_HEIGHT, 0, SDL_SWSURFACE);
		assert(screen != NULL);
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
	SDL_Surface *tint_tile = ImgStuff::GetImage("graphics/hextile.png", ImgStuff::TintValues(0,100,0));
	SDL_Surface *line_tile = ImgStuff::GetImage("graphics/hextile.png", ImgStuff::TintValues(0,20,0));
	SDL_Surface *pickup = ImgStuff::GetImage("graphics/pickup.png");
	
	TTF_Font *font = FontStuff::LoadFont("fonts/DejaVuSansMono.ttf", 14);
	TTF_Font *bfont = FontStuff::LoadFont("fonts/DejaVuSansMono-Bold.ttf", 14);
	
	if (current_animator) {
		current_animator->do_stuff();
	}
	
	assert(SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 0, 0, 0)) != -1);
	
	{
		SDL_Rect rect = {0,0,0,0};
		
		FontStuff::BlitText(screen, rect, font, ImgStuff::Colour(255,255,255), "Players: ");
		rect.x += FontStuff::TextWidth(font, "Players: ");
		
		player_set::iterator p = players.begin();
		
		for(; p != players.end(); p++) {
			if((*p).colour >= SPECTATE) {
				continue;
			}
			
			TTF_Font *f = (*p).id == turn ? bfont : font;
			
			FontStuff::BlitText(screen, rect, f, team_colours[(*p).colour], (*p).name + " ");
			rect.x += FontStuff::TextWidth(f, (*p).name + " ");
		}
	}
	
	Tile::List::iterator ti = tiles.begin();
	
	int mouse_x, mouse_y;
	SDL_GetMouseState(&mouse_x, &mouse_y);
	Tile *htile = TileAtXY(tiles, mouse_x, mouse_y);
	
	int bs_col, fs_col, diag_row = -1;
	
	for(; ti != tiles.end(); ti++) {
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
		
		SDL_Surface *tile_img = tile;
		
		if(htile == *ti) {
			tile_img = tint_tile;
		}else if(htile && options.show_lines) {
			if(diag_row != (*ti)->row) {
				diag_cols(htile, (*ti)->row, bs_col, fs_col);
				diag_row = (*ti)->row;
			}
			
			if((*ti)->col == bs_col || (*ti)->col == fs_col || (*ti)->row == htile->row) {
				tile_img = line_tile;
			}
		}
		
		assert(SDL_BlitSurface(tile_img, NULL, screen, &rect) == 0);
		
		if((*ti)->has_power) {
			assert(SDL_BlitSurface(pickup, NULL, screen, &rect) == 0);
		}
		
		if((*ti)->render_pawn && (*ti)->render_pawn != dpawn) {
			draw_pawn_tile((*ti)->render_pawn, *ti);
		}else if((*ti)->pawn && (*ti)->pawn != dpawn) {
			draw_pawn_tile((*ti)->pawn, *ti);
		}
	}
	
	for(anim_set::iterator ait = animators.begin(); ait != animators.end();) {
		anim_set::iterator anim = ait++;
		
		if(!(*anim)->render()) {
			animators.erase(anim);
		}
	}
	
	if(dpawn) {
		SDL_Rect rect = {mouse_x-30, mouse_y-30, 0, 0}, base = {0,0,50,50};
		DrawPawn(dpawn, rect, base);
	}
	
	pmenu.clear();
	pmenu_area.w = 0;
	pmenu_area.h = 0;
	
	if(mpawn) {
		int fh = TTF_FontLineSkip(font), fw = 0;
		
		Pawn::PowerList::iterator i = mpawn->powers.begin();
		
		for(; i != mpawn->powers.end(); i++) {
			int w = FontStuff::TextWidth(font, Powers::powers[i->first].name);
			
			if(w > fw) {
				fw = w;
			}
		}
		
		SDL_Rect rect = { mpawn->cur_tile->screen_x+TILE_WIDTH, mpawn->cur_tile->screen_y, fw+30, mpawn->powers.size() * fh };
		
		if(rect.x+rect.w > screen_w) {
			rect.x = mpawn->cur_tile->screen_x-rect.w;
		}
		if(rect.y+rect.h > screen_h) {
			rect.y = mpawn->cur_tile->screen_y-rect.h;
		}
		
		assert(SDL_FillRect(screen, &rect, SDL_MapRGB(screen->format, 0, 0, 0)) != -1);
		
		pmenu_area = rect;
		rect.h = fh;
		
		SDL_Color colour = {255,0,0};
		
		for(i = mpawn->powers.begin(); i != mpawn->powers.end(); i++) {
			pmenu_entry foobar = {rect, i->first};
			pmenu.push_back(foobar);
			
			FontStuff::BlitText(screen, rect, font, colour, to_string(i->second));
			
			rect.x += 30;
			
			FontStuff::BlitText(screen, rect, font, colour, Powers::powers[i->first].name);
			
			rect.x -= 30;
			rect.y += fh;
		}
	}
	
	SDL_UpdateRect(screen, 0, 0, 0, 0);
}

void Client::DrawPawn(Pawn *pawn, SDL_Rect rect, SDL_Rect base) {
	SDL_Surface *pawn_graphics = ImgStuff::GetImage("graphics/pawns.png");
	SDL_Surface *range_overlay = ImgStuff::GetImage("graphics/upgrades/range.png");
	SDL_Surface *shadow = ImgStuff::GetImage("graphics/shadow.png");
	SDL_Surface *shield = ImgStuff::GetImage("graphics/upgrades/shield.png");
	
	unsigned int frame = torus_frame;
	
	assert(SDL_BlitSurface(shadow, &base, screen, &rect) == 0);
	
	if(pawn->flags & PWR_CLIMB && pawn != dpawn) {
		rect.x -= climb_offset;
		rect.y -= climb_offset;
	}
	
	if(pawn == hpawn && pawn->colour == my_colour) {
		frame = 10;
	}
	else if(!(pawn->flags & HAS_POWER)) {
		frame = 0;
	}
	
	SDL_Rect srect = { frame * 50, (pawn->colour * 50) + base.y, 50, base.h };
	assert(SDL_BlitSurface(pawn_graphics, &srect, screen, &rect) == 0);
	
	srect.x = pawn->range * 50;
	srect.y = (pawn->colour * 50) + base.y;
	assert(SDL_BlitSurface(range_overlay, &srect, screen, &rect) == 0);
	
	if(pawn->flags & PWR_SHIELD) {
		assert(SDL_BlitSurface(shield, &base, screen, &rect) == 0);
	}
}

void Client::draw_pawn_tile(Pawn *pawn, Tile *tile) {
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
			pawn->last_tile->render_pawn = NULL;
			pawn->last_tile = NULL;
		}
	}
	
	if(pawn->cur_tile != tile && pawn->last_tile != tile) {
		return;
	}
	
	DrawPawn(pawn, rect, base);
}

static bool ccolour_callback(const GUI::DropDown &dropdown, const GUI::DropDown::Item &item, void *arg) {
	Client *client = (Client*)arg;
	client->change_colour(item.i1, (PlayerColour)item.i2);
	
	return false;
}

void Client::lobby_regen() {
	int y = 65;
	
	lobby_players.clear();
	lobby_drops.clear();
	
	for(player_set::iterator p = players.begin(); p != players.end(); p++) {
		boost::shared_ptr<GUI::TextButton> pn(new GUI::TextButton(lobby_gui, 20, y, 300, 35, 0, (*p).name));
		pn->align(GUI::LEFT);
		lobby_players.push_back(pn);
		
		if(my_id == p->id || my_id == ADMIN_ID) {
			boost::shared_ptr<GUI::DropDown> pc(new GUI::DropDown(lobby_gui, 330, y, 135, 35, y));
			
			for(int i = 0; i < 7; i++) {
				pc->items.push_back(GUI::DropDown::Item(team_names[i], team_colours[i], p->id, i));
			}
			
			pc->select(pc->items.begin()+p->colour);
			pc->callback = &ccolour_callback;
			pc->callback_arg = this;
			
			lobby_drops.push_back(pc);
		}else{
			boost::shared_ptr<GUI::TextButton> pc(new GUI::TextButton(lobby_gui, 330, y, 135, 35, 0, team_names[p->colour]));
			pc->align(GUI::LEFT);
			pc->set_fg_colour(team_colours[p->colour]);
			
			lobby_players.push_back(pc);
		}
		
		y += 40;
	}
}

void Client::send_begin() {
	protocol::message msg;
	msg.set_msg(protocol::BEGIN);
	
	WriteProto(msg);
}

void Client::change_colour(uint16_t id, PlayerColour colour) {
	protocol::message msg;
	msg.set_msg(protocol::CCOLOUR);
	
	msg.add_players();
	msg.mutable_players(0)->set_id(id);
	msg.mutable_players(0)->set_colour((protocol::colour)colour);
	
	WriteProto(msg);
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
