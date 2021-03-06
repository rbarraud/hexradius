#ifndef GUI_HPP
#define GUI_HPP

#include "loadimage.hpp"
#include "fontstuff.hpp"
#include "hexradius.hpp"

#include <string>
#include <SDL/SDL.h>
#include <set>
#include <SDL/SDL_ttf.h>
#include <vector>
#include <boost/shared_ptr.hpp>
#include <boost/function.hpp>
#include <boost/bind.hpp>
#include <map>

class GUI {
public:

	enum alignment { LEFT, CENTER, RIGHT };

	struct Thing {
		virtual void HandleEvent(const SDL_Event &event);
		virtual void Draw();
		virtual ~Thing();

		GUI &gui;

		int x, y, w, h;
		int tab_order;
		bool enabled;

		Thing(GUI &g) : gui(g), enabled(true) {}

		void enable(bool enable) {
			enabled = enable;
		}

		bool has_focus() {
			return gui.v_focus && *(gui.focus) == this;
		}
	};

	struct thing_compare {
		bool operator()(const Thing *left, const Thing *right) {
			if(left->tab_order == 0 && right->tab_order == 0) {
				return left < right;
			}

			return left->tab_order < right->tab_order;
		}
	};

	struct ImgButton : Thing {
		typedef boost::function<void(const ImgButton &, const SDL_Event &)> callback_t;

		callback_t onclick_callback;

		SDL_Surface *image;

		int x_down, y_down;

		ImgButton(GUI &_gui, SDL_Surface *img, int ax, int ay, int to, callback_t cb = callback_t());
		~ImgButton();

		void HandleEvent(const SDL_Event &event);
		void Draw();
	};

	struct TextBox : Thing {
		typedef boost::function<void(const TextBox &tbox, const SDL_Event &event)> enter_callback_t;
		typedef boost::function<bool(const TextBox &tbox, const SDL_Event &event)> input_callback_t;

		std::string text;
		unsigned int insert_offset;

		enter_callback_t enter_callback;
		input_callback_t input_callback;

		TextBox(GUI &g, int ax, int ay, int aw, int ah, int to);
		~TextBox();

		void set_text(const std::string &new_text);

		void set_enter_callback(enter_callback_t callback) {
			enter_callback = callback;
		}

		void set_input_callback(input_callback_t callback) {
			input_callback = callback;
		}

		void HandleEvent(const SDL_Event &event);
		void Draw();
	};

	struct TextDisplay : Thing {
		std::string text;
		TTF_Font *font;
		SDL_Colour colour;

		TextDisplay(GUI &g, int ax, int ay, std::string txt = "");
		~TextDisplay();

		void set_font(std::string name, int size) {
			font = FontStuff::LoadFont(name, size);
		}

		void Draw();
	};

	struct TextButton : Thing {
		typedef boost::function<void(const TextButton &, const SDL_Event &event)> callback_t;

		std::string m_text;
		TTF_Font *m_font;
		alignment m_align;
		SDL_Colour m_fgc;
		SDL_Colour m_bgc;
		bool m_borders;

		uint8_t m_opacity;
		SDL_Surface *m_bgs;

		callback_t m_callback;

		int x_down, y_down;

		TextButton(GUI &g, int ax, int ay, int aw, int ah, int to, std::string text, callback_t callback = callback_t());
		~TextButton();

		void align(alignment align) {
			m_align = align;
		}

		void set_fg_colour(int r, int g, int b) {
			m_fgc = ImgStuff::Colour(r, g, b);
		}

		void set_fg_colour(const SDL_Colour &colour) {
			m_fgc = colour;
		}

		void set_bg_colour(int r, int g, int b) {
			m_bgc = ImgStuff::Colour(r, g, b);
			assert(SDL_FillRect(m_bgs, NULL, ImgStuff::MapColour(m_bgc)) == 0);
		}

		void set_bg_colour(const SDL_Colour &colour) {
			m_bgc = colour;
			assert(SDL_FillRect(m_bgs, NULL, ImgStuff::MapColour(m_bgc)) == 0);
		}

		void Draw();
		void HandleEvent(const SDL_Event &event);
	};

	template<class key_type> class DropDown: Thing
	{
		private:
			TextButton button;

			std::vector<key_type>           item_keys;
			std::map<key_type, std::string> item_labels;
			std::map<key_type, SDL_Colour>  item_colours;

			key_type *selected_key;

			std::vector<boost::shared_ptr<TextButton> > item_buttons;

			void user_select(const key_type &key);

		public:
			typedef boost::function<bool(const key_type &key)> callback_t;

			callback_t callback;

			DropDown(GUI &g, int ax, int ay, int aw, int ah, int to);
			~DropDown();

			void Draw();
			void HandleEvent(const SDL_Event &event);

			void add_item(const key_type &key, const std::string &label, const SDL_Colour colour = ImgStuff::Colour(255, 255, 255));
			void del_item(const key_type &key);

			void select(const key_type &key);
			const key_type *selected();
	};

	struct Checkbox : Thing {
		typedef boost::function<void(const GUI::Checkbox &checkbox)> callback_t;

		bool enabled;
		bool state;

		int x_down, y_down;

		callback_t toggle_callback;

		Checkbox(GUI &g, int ax, int ay, int aw, int ah, int to, bool default_state = false, bool enabled = true);
		~Checkbox();

		void set_callback(callback_t callback);

		void Draw();
		void HandleEvent(const SDL_Event &event);
	};

	typedef std::set<Thing*,thing_compare> thing_set;

public:
	typedef boost::function<void(const GUI &drop, const SDL_Event &event)> callback_t;

	GUI(int ax, int ay, int aw, int ah);

	void set_bg_colour(int r, int g, int b);
	void set_bg_image(SDL_Surface *img);

	void set_quit_callback(callback_t callback);

	void poll(bool read_events);
	void redraw();
	void handle_event(const SDL_Event &event);

private:
	int x, y, w, h;
	Uint32 bgcolour;
	SDL_Surface *bgimg;

	thing_set things;

	bool v_focus;
	thing_set::iterator focus;

	callback_t quit_callback;

	void add_thing(Thing *thing);
	void del_thing(Thing *thing);
	void focus_next();
};

template <class key_type> void GUI::DropDown<key_type>::user_select(const key_type &key)
{
	if(!callback || callback(key))
	{
		select(key);
	}

	item_buttons.clear();
}

template <class key_type> GUI::DropDown<key_type>::DropDown(GUI &g, int ax, int ay, int aw, int ah, int to) : Thing(g), button(g, ax, ay, aw-ah, ah, 0, "UNSET")
{
	x = gui.x + ax;
	y = gui.y + ay;
	w = aw;
	h = ah;
	tab_order = to;

	button.align(LEFT);
	button.set_fg_colour(255, 0, 0);

	selected_key = NULL;

	callback = 0;

	gui.add_thing(this);
}

template <class key_type> GUI::DropDown<key_type>::~DropDown()
{
	gui.del_thing(this);

	delete selected_key;
	selected_key = NULL;
}

template <class key_type> void GUI::DropDown<key_type>::Draw()
{
	SDL_Rect rect = {x+w-h, y, h, h};

	ensure_SDL_FillRect(screen, &rect, SDL_MapRGB(screen->format, 0, 0, 0));

	Uint32 bcolour = has_focus() ? SDL_MapRGB(screen->format, 255, 255, 0) : SDL_MapRGB(screen->format, 255, 255, 255);
	SDL_Rect ra = {x,y,w,1}, rb = {x,y,1,h}, rc = {x,y+h,w,1}, rd = {x+w,y,1,h+1}, re = {x+w-h,y,1,h};

	ensure_SDL_FillRect(screen, &ra, bcolour);
	ensure_SDL_FillRect(screen, &rb, bcolour);
	ensure_SDL_FillRect(screen, &rc, bcolour);
	ensure_SDL_FillRect(screen, &rd, bcolour);
	ensure_SDL_FillRect(screen, &re, bcolour);
}

template <class key_type> void GUI::DropDown<key_type>::HandleEvent(const SDL_Event &event)
{
	if(event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT)
	{
		if(item_buttons.empty())
		{
			int ty = y+h;
			int to = 2000;

			for(typename std::vector<key_type>::iterator i = item_keys.begin(); i != item_keys.end(); ++i)
			{
				std::string text  = item_labels.find(*i)->second;
				SDL_Colour colour = item_colours.find(*i)->second;

				boost::shared_ptr<TextButton> btn(new TextButton(gui, x, ty, w, h, to, text, boost::bind(&GUI::DropDown<key_type>::user_select, this, *i)));
				btn->set_fg_colour(colour);
				btn->align(LEFT);
				btn->m_borders = false;

				item_buttons.push_back(btn);

				ty += h;
				to++;
			}
		}
		else{
			item_buttons.clear();
		}
	}
	else if(event.type == SDL_MOUSEMOTION)
	{
		std::vector<boost::shared_ptr<TextButton> >::iterator i= item_buttons.begin();

		for(; i != item_buttons.end(); i++)
		{
			int mx = event.motion.x;
			int my = event.motion.y;

			if(mx >= (*i)->x && mx < (*i)->x+w && my >= (*i)->y && my < (*i)->y+h)
			{
				(*i)->set_bg_colour(45, 45, 0);
			}
			else{
				(*i)->set_bg_colour(0, 0, 0);
			}
		}
	}
	else if(event.type == SDL_KEYDOWN)
	{
		if(event.key.keysym.sym == SDLK_ESCAPE)
		{
			item_buttons.clear();
		}
	}
}

template <class key_type> void GUI::DropDown<key_type>::add_item(const key_type &key, const std::string &label, const SDL_Colour colour)
{
	if(item_labels.find(key) != item_labels.end())
	{
		return;
	}

	item_keys.push_back(key);
	item_labels.insert(std::make_pair(key, label));
	item_colours.insert(std::make_pair(key, colour));
}

template <class key_type> void GUI::DropDown<key_type>::del_item(const key_type &key)
{
	if(selected_key && *selected_key == key)
	{
		delete selected_key;
		selected_key = NULL;
	}

	item_labels.erase(key);
	item_colours.erase(key);

	for(typename std::vector<key_type>::iterator i = item_keys.begin(); i != item_keys.end(); ++i)
	{
		if(*i == key)
		{
			item_keys.erase(i);
			break;
		}
	}
}

template <class key_type> void GUI::DropDown<key_type>::select(const key_type &key)
{
	if(item_labels.find(key) == item_labels.end())
	{
		return;
	}

	if(selected_key)
	{
		*selected_key = key;
	}
	else{
		selected_key = new key_type(key);
	}

	button.m_text = item_labels.find(key)->second;
	button.set_fg_colour(item_colours.find(key)->second);
}

template <class key_type> const key_type *GUI::DropDown<key_type>::selected()
{
	return selected_key;
}

#endif /* !GUI_HPP */
