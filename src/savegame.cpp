#include "game.h" // IWYU pragma: associated

#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include <type_traits>
#include <unordered_set>
#include <utility>

#include "avatar.h"
#include "coordinate_conversions.h"
#include "creature_tracker.h"
#include "debug.h"
#include "faction.h"
#include "io.h"
#include "map.h"
#include "messages.h"
#include "mission.h"
#include "mongroup.h"
#include "monster.h"
#include "npc.h"
#include "options.h"
#include "output.h"
#include "overmap.h"
#include "scent_map.h"
#include "translations.h"
#include "tuple_hash.h"
#include "basecamp.h"
#include "json.h"
#include "omdata.h"
#include "overmap_types.h"
#include "player.h"
#include "regional_settings.h"
#include "int_id.h"
#include "string_id.h"

#if defined(__ANDROID__)
#include "input.h"

extern std::map<std::string, std::list<input_event>> quick_shortcuts_map;
#endif

/*
 * Changes that break backwards compatibility should bump this number, so the game can
 * load a legacy format loader.
 */
const int savegame_version = 25;

/*
 * This is a global set by detected version header in .sav, maps.txt, or overmap.
 * This allows loaders for classes that exist in multiple files (such as item) to have
 * support for backwards compatibility as well.
 */
int savegame_loading_version = savegame_version;

/*
 * Save to opened character.sav
 */
void game::serialize( std::ostream &fout )
{
    /*
     * Format version 12: Fully json, save the header. Weather and memorial exist elsewhere.
     * To prevent (or encourage) confusion, there is no version 8. (cata 0.8 uses v7)
     */
    // Header
    fout << "# version " << savegame_version << std::endl;

    JsonOut json( fout, true ); // pretty-print

    json.start_object();
    // basic game state information.
    json.member( "turn", static_cast<int>( calendar::turn ) );
    json.member( "calendar_start", static_cast<int>( calendar::start ) );
    json.member( "initial_season", static_cast<int>( calendar::initial_season ) );
    json.member( "auto_travel_mode", auto_travel_mode );
    json.member( "run_mode", static_cast<int>( safe_mode ) );
    json.member( "mostseen", mostseen );
    // current map coordinates
    tripoint pos_sm = m.get_abs_sub();
    const point pos_om = sm_to_om_remain( pos_sm.x, pos_sm.y );
    json.member( "levx", pos_sm.x );
    json.member( "levy", pos_sm.y );
    json.member( "levz", pos_sm.z );
    json.member( "om_x", pos_om.x );
    json.member( "om_y", pos_om.y );

    json.member( "grscent", scent.serialize() );

    // Then each monster
    json.member( "active_monsters", *critter_tracker );
    json.member( "stair_monsters", coming_to_stairs );

    // save killcounts.
    json.member( "kills" );
    json.start_object();
    for( auto &elem : kills ) {
        json.member( elem.first.str(), elem.second );
    }
    json.end_object();

    json.member( "npc_kills" );
    json.start_array();
    for( auto &elem : npc_kills ) {
        json.write( elem );
    }
    json.end_array();

    json.member( "player", u );
    Messages::serialize( json );

    json.end_object();
}

std::string scent_map::serialize() const
{
    std::stringstream rle_out;
    int rle_lastval = -1;
    int rle_count = 0;
    for( auto &elem : grscent ) {
        for( auto &val : elem ) {
            if( val == rle_lastval ) {
                rle_count++;
            } else {
                if( rle_count ) {
                    rle_out << rle_count << " ";
                }
                rle_out << val << " ";
                rle_lastval = val;
                rle_count = 1;
            }
        }
    }
    rle_out << rle_count;
    return rle_out.str();
}

static void chkversion( std::istream &fin )
{
    if( fin.peek() == '#' ) {
        std::string vline;
        getline( fin, vline );
        std::string tmphash;
        std::string tmpver;
        int savedver = -1;
        std::stringstream vliness( vline );
        vliness >> tmphash >> tmpver >> savedver;
        if( tmpver == "version" && savedver != -1 ) {
            savegame_loading_version = savedver;
        }
    }
}

/*
 * Parse an open .sav file.
 */
void game::unserialize( std::istream &fin )
{
    if( fin.peek() == '#' ) {
        std::string vline;
        getline( fin, vline );
        std::string tmphash;
        std::string tmpver;
        int savedver = -1;
        std::stringstream vliness( vline );
        vliness >> tmphash >> tmpver >> savedver;
        if( tmpver == "version" && savedver != -1 ) {
            savegame_loading_version = savedver;
        }
    }
    std::string linebuf;

    int tmpturn = 0;
    int tmpcalstart = 0;
    int tmprun = 0;
    int levx = 0;
    int levy = 0;
    int levz = 0;
    int comx = 0;
    int comy = 0;
    JsonIn jsin( fin );
    try {
        JsonObject data = jsin.get_object();

        data.read( "turn", tmpturn );
        data.read( "calendar_start", tmpcalstart );
        calendar::initial_season = static_cast<season_type>( data.get_int( "initial_season",
                                   static_cast<int>( SPRING ) ) );
        data.read( "auto_travel_mode", auto_travel_mode );
        data.read( "run_mode", tmprun );
        data.read( "mostseen", mostseen );
        data.read( "levx", levx );
        data.read( "levy", levy );
        data.read( "levz", levz );
        data.read( "om_x", comx );
        data.read( "om_y", comy );

        calendar::turn = tmpturn;
        calendar::start = tmpcalstart;

        load_map( tripoint( levx + comx * OMAPX * 2, levy + comy * OMAPY * 2, levz ) );

        safe_mode = static_cast<safe_mode_type>( tmprun );
        if( get_option<bool>( "SAFEMODE" ) && safe_mode == SAFE_MODE_OFF ) {
            safe_mode = SAFE_MODE_ON;
        }

        linebuf.clear();
        if( data.read( "grscent", linebuf ) ) {
            scent.deserialize( linebuf );
        } else {
            scent.reset();
        }

        data.read( "active_monsters", *critter_tracker );

        JsonArray vdata = data.get_array( "stair_monsters" );
        coming_to_stairs.clear();
        while( vdata.has_more() ) {
            monster stairtmp;
            vdata.read_next( stairtmp );
            coming_to_stairs.push_back( stairtmp );
        }

        JsonObject odata = data.get_object( "kills" );
        std::set<std::string> members = odata.get_member_names();
        for( const auto &member : members ) {
            kills[mtype_id( member )] = odata.get_int( member );
        }

        vdata = data.get_array( "npc_kills" );
        while( vdata.has_more() ) {
            std::string npc_name;
            vdata.read_next( npc_name );
            npc_kills.push_back( npc_name );
        }

        data.read( "player", u );
        Messages::deserialize( data );

    } catch( const JsonError &jsonerr ) {
        debugmsg( "Bad save json\n%s", jsonerr.c_str() );
        return;
    }
}

void scent_map::deserialize( const std::string &data )
{
    std::istringstream buffer( data );
    int stmp = 0;
    int count = 0;
    for( auto &elem : grscent ) {
        for( auto &val : elem ) {
            if( count == 0 ) {
                buffer >> stmp >> count;
            }
            count--;
            val = stmp;
        }
    }
}

///// weather
void game::load_weather( std::istream &fin )
{
    if( fin.peek() == '#' ) {
        std::string vline;
        getline( fin, vline );
        std::string tmphash;
        std::string tmpver;
        int savedver = -1;
        std::stringstream vliness( vline );
        vliness >> tmphash >> tmpver >> savedver;
        if( tmpver == "version" && savedver != -1 ) {
            savegame_loading_version = savedver;
        }
    }

    //Check for "lightning:" marker - if absent, ignore
    if( fin.peek() == 'l' ) {
        std::string line;
        getline( fin, line );
        weather.lightning_active = ( line.compare( "lightning: 1" ) == 0 );
    } else {
        weather.lightning_active = false;
    }
    if( fin.peek() == 's' ) {
        std::string line;
        std::string label;
        getline( fin, line );
        std::stringstream liness( line );
        liness >> label >> seed;
    }
}

void game::save_weather( std::ostream &fout )
{
    fout << "# version " << savegame_version << std::endl;
    fout << "lightning: " << ( weather.lightning_active ? "1" : "0" ) << std::endl;
    fout << "seed: " << seed;
}

#if defined(__ANDROID__)
///// quick shortcuts
void game::load_shortcuts( std::istream &fin )
{
    std::string linebuf;
    std::stringstream linein;

    JsonIn jsin( fin );
    try {
        JsonObject data = jsin.get_object();

        if( get_option<bool>( "ANDROID_SHORTCUT_PERSISTENCE" ) ) {
            JsonObject qs = data.get_object( "quick_shortcuts" );
            std::set<std::string> qsl_members = qs.get_member_names();
            quick_shortcuts_map.clear();
            for( std::set<std::string>::iterator it = qsl_members.begin();
                 it != qsl_members.end(); ++it ) {
                JsonArray ja = qs.get_array( *it );
                std::list<input_event> &qslist = quick_shortcuts_map[ *it ];
                qslist.clear();
                while( ja.has_more() ) {
                    qslist.push_back( input_event( ja.next_int(), CATA_INPUT_KEYBOARD ) );
                }
            }
        }
    } catch( const JsonError &jsonerr ) {
        debugmsg( "Bad shortcuts json\n%s", jsonerr.c_str() );
        return;
    }
}

void game::save_shortcuts( std::ostream &fout )
{
    JsonOut json( fout, true ); // pretty-print

    json.start_object();
    if( get_option<bool>( "ANDROID_SHORTCUT_PERSISTENCE" ) ) {
        json.member( "quick_shortcuts" );
        json.start_object();
        for( auto &e : quick_shortcuts_map ) {
            json.member( e.first );
            const std::list<input_event> &qsl = e.second;
            json.start_array();
            for( const auto &event : qsl ) {
                json.write( event.get_first_input() );
            }
            json.end_array();
        }
        json.end_object();
    }
    json.end_object();
}
#endif

std::unordered_set<std::string> obsolete_terrains;

void overmap::load_obsolete_terrains( JsonObject &jo )
{
    JsonArray ja = jo.get_array( "terrains" );
    while( ja.has_more() ) {
        obsolete_terrains.emplace( ja.next_string() );
    }
}

bool overmap::obsolete_terrain( const std::string &ter )
{
    return obsolete_terrains.find( ter ) != obsolete_terrains.end();
}

/*
 * Complex conversion of outdated overmap terrain ids.
 * This is used when loading saved games with old oter_ids.
 */
void overmap::convert_terrain( const std::unordered_map<tripoint, std::string> &needs_conversion )
{
    for( const auto &convert : needs_conversion ) {
        const tripoint pos = convert.first;
        const std::string old = convert.second;
        oter_id &new_id = ter( pos.x, pos.y, pos.z );

        struct convert_nearby {
            int xoffset;
            std::string x_id;
            int yoffset;
            std::string y_id;
            std::string new_id;
        };

        std::vector<convert_nearby> nearby;

        if( old == "apartments_con_tower_1_entrance" ||
            old == "apartments_mod_tower_1_entrance" ) {
            const std::string base = old.substr( 0, old.rfind( "1_entrance" ) );
            const std::string other = base + "1";
            nearby.push_back( { 1, other, -1, other, base + "SW_north" } );
            nearby.push_back( { -1, other, 1, other, base + "SW_south" } );
            nearby.push_back( { 1, other, 1, other, base + "SW_east" } );
            nearby.push_back( { -1, other, -1, other, base + "SW_west" } );

        } else if( old == "apartments_con_tower_1" || old == "apartments_mod_tower_1" ) {
            const std::string base = old.substr( 0, old.rfind( '1' ) );
            const std::string entr = base + "1_entrance";
            nearby.push_back( { 1, old, 1, entr, base + "NW_north" } );
            nearby.push_back( { -1, old, -1, entr, base + "NW_south" } );
            nearby.push_back( { -1, entr, 1, old, base + "NW_east" } );
            nearby.push_back( { 1, entr, -1, old, base + "NW_west" } );
            nearby.push_back( { -1, old, 1, old, base + "NE_north" } );
            nearby.push_back( { 1, old, -1, old, base + "NE_south" } );
            nearby.push_back( { -1, old, -1, old, base + "NE_east" } );
            nearby.push_back( { 1, old, 1, old, base + "NE_west" } );
            nearby.push_back( { -1, entr, -1, old, base + "SE_north" } );
            nearby.push_back( { 1, entr, 1, old, base + "SE_south" } );
            nearby.push_back( { 1, old, -1, entr, base + "SE_east" } );
            nearby.push_back( { -1, old, 1, entr, base + "SE_west" } );

        } else if( old == "subway_station" ) {
            new_id = oter_id( "underground_sub_station" );
        } else if( old == "bridge_ew" ) {
            new_id = oter_id( "bridge_east" );
        } else if( old == "bridge_ns" ) {
            new_id = oter_id( "bridge_north" );
        } else if( old == "public_works_entrance" ) {
            const std::string base = "public_works_";
            const std::string other = "public_works";
            nearby.push_back( { 1, other, -1, other, base + "SW_north" } );
            nearby.push_back( { -1, other, 1, other, base + "SW_south" } );
            nearby.push_back( { 1, other, 1, other, base + "SW_east" } );
            nearby.push_back( { -1, other, -1, other, base + "SW_west" } );

        } else if( old == "public_works" ) {
            const std::string base = "public_works_";
            const std::string entr = "public_works_entrance";
            nearby.push_back( { 1, old, 1, entr, base + "NW_north" } );
            nearby.push_back( { -1, old, -1, entr, base + "NW_south" } );
            nearby.push_back( { -1, entr, 1, old, base + "NW_east" } );
            nearby.push_back( { 1, entr, -1, old, base + "NW_west" } );
            nearby.push_back( { -1, old, 1, old, base + "NE_north" } );
            nearby.push_back( { 1, old, -1, old, base + "NE_south" } );
            nearby.push_back( { -1, old, -1, old, base + "NE_east" } );
            nearby.push_back( { 1, old, 1, old, base + "NE_west" } );
            nearby.push_back( { -1, entr, -1, old, base + "SE_north" } );
            nearby.push_back( { 1, entr, 1, old, base + "SE_south" } );
            nearby.push_back( { 1, old, -1, entr, base + "SE_east" } );
            nearby.push_back( { -1, old, 1, entr, base + "SE_west" } );

        } else if( old.compare( 0, 7, "school_" ) == 0 ) {
            const std::string school = "school_";
            const std::string school_1 = school + "1_";
            if( old == school + "1" ) {
                nearby.push_back( { -1, school + "2", 1, school + "4", school_1 + "1_north" } );
                nearby.push_back( { -1, school + "4", -1, school + "2", school_1 + "1_east" } );
                nearby.push_back( { 1, school + "2", -1, school + "4", school_1 + "1_south" } );
                nearby.push_back( { 1, school + "4", 1, school + "2", school_1 + "1_west" } );
            } else if( old == school + "2" ) {
                nearby.push_back( { -1, school + "3", 1, school + "5", school_1 + "2_north" } );
                nearby.push_back( { -1, school + "5", -1, school + "3", school_1 + "2_east" } );
                nearby.push_back( { 1, school + "3", -1, school + "5", school_1 + "2_south" } );
                nearby.push_back( { 1, school + "5", 1, school + "3", school_1 + "2_west" } );
            } else if( old == school + "3" ) {
                nearby.push_back( { 1, school + "2", 1, school + "6", school_1 + "3_north" } );
                nearby.push_back( { -1, school + "6", 1, school + "2", school_1 + "3_east" } );
                nearby.push_back( { -1, school + "2", -1, school + "6", school_1 + "3_south" } );
                nearby.push_back( { 1, school + "6", -1, school + "2", school_1 + "3_west" } );
            } else if( old == school + "4" ) {
                nearby.push_back( { -1, school + "5", 1, school + "7", school_1 + "4_north" } );
                nearby.push_back( { -1, school + "7", -1, school + "5", school_1 + "4_east" } );
                nearby.push_back( { 1, school + "5", -1, school + "7", school_1 + "4_south" } );
                nearby.push_back( { 1, school + "7", 1, school + "5", school_1 + "4_west" } );
            } else if( old == school + "5" ) {
                nearby.push_back( { -1, school + "6", 1, school + "8", school_1 + "5_north" } );
                nearby.push_back( { -1, school + "8", -1, school + "6", school_1 + "5_east" } );
                nearby.push_back( { 1, school + "6", -1, school + "8", school_1 + "5_south" } );
                nearby.push_back( { 1, school + "8", 1, school + "6", school_1 + "5_west" } );
            } else if( old == school + "6" ) {
                nearby.push_back( { 1, school + "5", 1, school + "9", school_1 + "6_north" } );
                nearby.push_back( { -1, school + "9", 1, school + "5", school_1 + "6_east" } );
                nearby.push_back( { -1, school + "5", -1, school + "9", school_1 + "6_south" } );
                nearby.push_back( { 1, school + "9", -1, school + "5", school_1 + "6_west" } );
            } else if( old == school + "7" ) {
                nearby.push_back( { -1, school + "8", -1, school + "4", school_1 + "7_north" } );
                nearby.push_back( { 1, school + "4", -1, school + "8", school_1 + "7_east" } );
                nearby.push_back( { 1, school + "8", 1, school + "4", school_1 + "7_south" } );
                nearby.push_back( { -1, school + "4", 1, school + "8", school_1 + "7_west" } );
            } else if( old == school + "8" ) {
                nearby.push_back( { -1, school + "9", -1, school + "5", school_1 + "8_north" } );
                nearby.push_back( { 1, school + "5", -1, school + "9", school_1 + "8_east" } );
                nearby.push_back( { 1, school + "9", 1, school + "5", school_1 + "8_south" } );
                nearby.push_back( { -1, school + "5", 1, school + "9", school_1 + "8_west" } );
            } else if( old == school + "9" ) {
                nearby.push_back( { 1, school + "8", -1, school + "6", school_1 + "9_north" } );
                nearby.push_back( { 1, school + "6", 1, school + "8", school_1 + "9_east" } );
                nearby.push_back( { -1, school + "8", 1, school + "6", school_1 + "9_south" } );
                nearby.push_back( { -1, school + "6", -1, school + "8", school_1 + "9_west" } );
            }

        } else if( old.compare( 0, 7, "prison_" ) == 0 ) {
            const std::string prison = "prison_";
            const std::string prison_1 = prison + "1_";
            if( old == "prison_b_entrance" ) {
                new_id = oter_id( "prison_1_b_2_north" );
            } else if( old == "prison_b" ) {
                if( pos.z < 0 ) {
                    nearby.push_back( { -1, "prison_b_entrance",  1, "prison_b",          "prison_1_b_1_north" } );
                    nearby.push_back( {  1, "prison_b_entrance",  1, "prison_b",          "prison_1_b_3_north" } );
                    nearby.push_back( { -2, "prison_b",           1, "prison_b",          "prison_1_b_4_north" } );
                    nearby.push_back( {  0, "prison_b",          -1, "prison_b_entrance", "prison_1_b_5_north" } );
                    nearby.push_back( {  2, "prison_b",           1, "prison_b",          "prison_1_b_6_north" } );
                    nearby.push_back( { -2, "prison_b",          -2, "prison_b",          "prison_1_b_7_north" } );
                    nearby.push_back( {  0, "prison_b",          -2, "prison_b_entrance", "prison_1_b_8_north" } );
                    nearby.push_back( {  2, "prison_b",          -2, "prison_b",          "prison_1_b_9_north" } );
                }
            } else if( old == prison + "1" ) {
                nearby.push_back( { -1, prison + "2", 1, prison + "4", prison_1 + "1_north" } );
                nearby.push_back( { -1, prison + "4", -1, prison + "2", prison_1 + "1_east" } );
                nearby.push_back( { 1, prison + "2", -1, prison + "4", prison_1 + "1_south" } );
                nearby.push_back( { 1, prison + "4", 1, prison + "2", prison_1 + "1_west" } );
            } else if( old == prison + "2" ) {
                nearby.push_back( { -1, prison + "3", 1, prison + "5", prison_1 + "2_north" } );
                nearby.push_back( { -1, prison + "5", -1, prison + "3", prison_1 + "2_east" } );
                nearby.push_back( { 1, prison + "3", -1, prison + "5", prison_1 + "2_south" } );
                nearby.push_back( { 1, prison + "5", 1, prison + "3", prison_1 + "2_west" } );
            } else if( old == prison + "3" ) {
                nearby.push_back( { 1, prison + "2", 1, prison + "6", prison_1 + "3_north" } );
                nearby.push_back( { -1, prison + "6", 1, prison + "2", prison_1 + "3_east" } );
                nearby.push_back( { -1, prison + "2", -1, prison + "6", prison_1 + "3_south" } );
                nearby.push_back( { 1, prison + "6", -1, prison + "2", prison_1 + "3_west" } );
            } else if( old == prison + "4" ) {
                nearby.push_back( { -1, prison + "5", 1, prison + "7", prison_1 + "4_north" } );
                nearby.push_back( { -1, prison + "7", -1, prison + "5", prison_1 + "4_east" } );
                nearby.push_back( { 1, prison + "5", -1, prison + "7", prison_1 + "4_south" } );
                nearby.push_back( { 1, prison + "7", 1, prison + "5", prison_1 + "4_west" } );
            } else if( old == prison + "5" ) {
                nearby.push_back( { -1, prison + "6", 1, prison + "8", prison_1 + "5_north" } );
                nearby.push_back( { -1, prison + "8", -1, prison + "6", prison_1 + "5_east" } );
                nearby.push_back( { 1, prison + "6", -1, prison + "8", prison_1 + "5_south" } );
                nearby.push_back( { 1, prison + "8", 1, prison + "6", prison_1 + "5_west" } );
            } else if( old == prison + "6" ) {
                nearby.push_back( { 1, prison + "5", 1, prison + "9", prison_1 + "6_north" } );
                nearby.push_back( { -1, prison + "9", 1, prison + "5", prison_1 + "6_east" } );
                nearby.push_back( { -1, prison + "5", -1, prison + "9", prison_1 + "6_south" } );
                nearby.push_back( { 1, prison + "9", -1, prison + "5", prison_1 + "6_west" } );
            } else if( old == prison + "7" ) {
                nearby.push_back( { -1, prison + "8", -1, prison + "4", prison_1 + "7_north" } );
                nearby.push_back( { 1, prison + "4", -1, prison + "8", prison_1 + "7_east" } );
                nearby.push_back( { 1, prison + "8", 1, prison + "4", prison_1 + "7_south" } );
                nearby.push_back( { -1, prison + "4", 1, prison + "8", prison_1 + "7_west" } );
            } else if( old == prison + "8" ) {
                nearby.push_back( { -1, prison + "9", -1, prison + "5", prison_1 + "8_north" } );
                nearby.push_back( { 1, prison + "5", -1, prison + "9", prison_1 + "8_east" } );
                nearby.push_back( { 1, prison + "9", 1, prison + "5", prison_1 + "8_south" } );
                nearby.push_back( { -1, prison + "5", 1, prison + "9", prison_1 + "8_west" } );
            } else if( old == prison + "9" ) {
                nearby.push_back( { 1, prison + "8", -1, prison + "6", prison_1 + "9_north" } );
                nearby.push_back( { 1, prison + "6", 1, prison + "8", prison_1 + "9_east" } );
                nearby.push_back( { -1, prison + "8", 1, prison + "6", prison_1 + "9_south" } );
                nearby.push_back( { -1, prison + "6", -1, prison + "8", prison_1 + "9_west" } );
            }

        } else if( old.compare( 0, 8, "hospital" ) == 0 ) {
            const std::string hospital = "hospital";
            const std::string hospital_entrance = "hospital_entrance";
            if( old == hospital_entrance ) {
                new_id = oter_id( hospital + "_2_north" );
            } else if( old == hospital ) {
                nearby.push_back( { -1, hospital_entrance,  1, hospital,          hospital + "_1_north" } );
                nearby.push_back( {  1, hospital_entrance,  1, hospital,          hospital + "_3_north" } );
                nearby.push_back( { -2, hospital,           1, hospital,          hospital + "_4_north" } );
                nearby.push_back( {  0, hospital,          -1, hospital_entrance, hospital + "_5_north" } );
                nearby.push_back( {  2, hospital,           1, hospital,          hospital + "_6_north" } );
                nearby.push_back( { -2, hospital,          -2, hospital,          hospital + "_7_north" } );
                nearby.push_back( {  0, hospital,          -2, hospital_entrance, hospital + "_8_north" } );
                nearby.push_back( {  2, hospital,          -2, hospital,          hospital + "_9_north" } );
            }
        } else if( old == "cathedral_1_entrance" ) {
            const std::string base = "cathedral_1_";
            const std::string other = "cathedral_1";
            nearby.push_back( { 1, other, -1, other, base + "SW_north" } );
            nearby.push_back( { -1, other, 1, other, base + "SW_south" } );
            nearby.push_back( { 1, other, 1, other, base + "SW_east" } );
            nearby.push_back( { -1, other, -1, other, base + "SW_west" } );

        } else if( old == "cathedral_1" ) {
            const std::string base = "cathedral_1_";
            const std::string entr = "cathedral_1_entrance";
            nearby.push_back( { 1, old, 1, entr, base + "NW_north" } );
            nearby.push_back( { -1, old, -1, entr, base + "NW_south" } );
            nearby.push_back( { -1, entr, 1, old, base + "NW_east" } );
            nearby.push_back( { 1, entr, -1, old, base + "NW_west" } );
            nearby.push_back( { -1, old, 1, old, base + "NE_north" } );
            nearby.push_back( { 1, old, -1, old, base + "NE_south" } );
            nearby.push_back( { -1, old, -1, old, base + "NE_east" } );
            nearby.push_back( { 1, old, 1, old, base + "NE_west" } );
            nearby.push_back( { -1, entr, -1, old, base + "SE_north" } );
            nearby.push_back( { 1, entr, 1, old, base + "SE_south" } );
            nearby.push_back( { 1, old, -1, entr, base + "SE_east" } );
            nearby.push_back( { -1, old, 1, entr, base + "SE_west" } );

        } else if( old == "cathedral_b_entrance" ) {
            const std::string base = "cathedral_b_";
            const std::string other = "cathedral_b";
            nearby.push_back( { 1, other, -1, other, base + "SW_north" } );
            nearby.push_back( { -1, other, 1, other, base + "SW_south" } );
            nearby.push_back( { 1, other, 1, other, base + "SW_east" } );
            nearby.push_back( { -1, other, -1, other, base + "SW_west" } );

        } else if( old == "cathedral_b" ) {
            const std::string base = "cathedral_b_";
            const std::string entr = "cathedral_b_entrance";
            nearby.push_back( { 1, old, 1, entr, base + "NW_north" } );
            nearby.push_back( { -1, old, -1, entr, base + "NW_south" } );
            nearby.push_back( { -1, entr, 1, old, base + "NW_east" } );
            nearby.push_back( { 1, entr, -1, old, base + "NW_west" } );
            nearby.push_back( { -1, old, 1, old, base + "NE_north" } );
            nearby.push_back( { 1, old, -1, old, base + "NE_south" } );
            nearby.push_back( { -1, old, -1, old, base + "NE_east" } );
            nearby.push_back( { 1, old, 1, old, base + "NE_west" } );
            nearby.push_back( { -1, entr, -1, old, base + "SE_north" } );
            nearby.push_back( { 1, entr, 1, old, base + "SE_south" } );
            nearby.push_back( { 1, old, -1, entr, base + "SE_east" } );
            nearby.push_back( { -1, old, 1, entr, base + "SE_west" } );

        } else if( old.compare( 0, 14, "hotel_tower_1_" ) == 0 ) {
            const std::string hotel = "hotel_tower_1_";
            if( old == hotel + "1" ) {
                nearby.push_back( { -1, hotel + "2", 1, hotel + "4", hotel + "1_north" } );
                nearby.push_back( { -1, hotel + "4", -1, hotel + "2", hotel + "1_east" } );
                nearby.push_back( { 1, hotel + "2", -1, hotel + "4", hotel + "1_south" } );
                nearby.push_back( { 1, hotel + "4", 1, hotel + "2", hotel + "1_west" } );
            } else if( old == hotel + "2" ) {
                nearby.push_back( { -1, hotel + "3", 1, hotel + "5", hotel + "2_north" } );
                nearby.push_back( { -1, hotel + "5", -1, hotel + "3", hotel + "2_east" } );
                nearby.push_back( { 1, hotel + "3", -1, hotel + "5", hotel + "2_south" } );
                nearby.push_back( { 1, hotel + "5", 1, hotel + "3", hotel + "2_west" } );
            } else if( old == hotel + "3" ) {
                nearby.push_back( { 1, hotel + "2", 1, hotel + "6", hotel + "3_north" } );
                nearby.push_back( { -1, hotel + "6", 1, hotel + "2", hotel + "3_east" } );
                nearby.push_back( { -1, hotel + "2", -1, hotel + "6", hotel + "3_south" } );
                nearby.push_back( { 1, hotel + "6", -1, hotel + "2", hotel + "3_west" } );
            } else if( old == hotel + "4" ) {
                nearby.push_back( { -1, hotel + "5", 1, hotel + "7", hotel + "4_north" } );
                nearby.push_back( { -1, hotel + "7", -1, hotel + "5", hotel + "4_east" } );
                nearby.push_back( { 1, hotel + "5", -1, hotel + "7", hotel + "4_south" } );
                nearby.push_back( { 1, hotel + "7", 1, hotel + "5", hotel + "4_west" } );
            } else if( old == hotel + "5" ) {
                nearby.push_back( { -1, hotel + "6", 1, hotel + "8", hotel + "5_north" } );
                nearby.push_back( { -1, hotel + "8", -1, hotel + "6", hotel + "5_east" } );
                nearby.push_back( { 1, hotel + "6", -1, hotel + "8", hotel + "5_south" } );
                nearby.push_back( { 1, hotel + "8", 1, hotel + "6", hotel + "5_west" } );
            } else if( old == hotel + "6" ) {
                nearby.push_back( { 1, hotel + "5", 1, hotel + "9", hotel + "6_north" } );
                nearby.push_back( { -1, hotel + "9", 1, hotel + "5", hotel + "6_east" } );
                nearby.push_back( { -1, hotel + "5", -1, hotel + "9", hotel + "6_south" } );
                nearby.push_back( { 1, hotel + "9", -1, hotel + "5", hotel + "6_west" } );
            } else if( old == hotel + "7" ) {
                nearby.push_back( { -1, hotel + "8", -1, hotel + "4", hotel + "7_north" } );
                nearby.push_back( { 1, hotel + "4", -1, hotel + "8", hotel + "7_east" } );
                nearby.push_back( { 1, hotel + "8", 1, hotel + "4", hotel + "7_south" } );
                nearby.push_back( { -1, hotel + "4", 1, hotel + "8", hotel + "7_west" } );
            } else if( old == hotel + "8" ) {
                nearby.push_back( { -1, hotel + "9", -1, hotel + "5", hotel + "8_north" } );
                nearby.push_back( { 1, hotel + "5", -1, hotel + "9", hotel + "8_east" } );
                nearby.push_back( { 1, hotel + "9", 1, hotel + "5", hotel + "8_south" } );
                nearby.push_back( { -1, hotel + "5", 1, hotel + "9", hotel + "8_west" } );
            } else if( old == hotel + "9" ) {
                nearby.push_back( { 1, hotel + "8", -1, hotel + "6", hotel + "9_north" } );
                nearby.push_back( { 1, hotel + "6", 1, hotel + "8", hotel + "9_east" } );
                nearby.push_back( { -1, hotel + "8", 1, hotel + "6", hotel + "9_south" } );
                nearby.push_back( { -1, hotel + "6", -1, hotel + "8", hotel + "9_west" } );
            }

        } else if( old.compare( 0, 14, "hotel_tower_b_" ) == 0 ) {
            const std::string hotelb = "hotel_tower_b_";
            if( old == hotelb + "1" ) {
                nearby.push_back( { -1, hotelb + "2", 0, hotelb + "1", hotelb + "1_north" } );
                nearby.push_back( { 0, hotelb + "1", -1, hotelb + "2", hotelb + "1_east" } );
                nearby.push_back( { 1, hotelb + "2", 0, hotelb + "1", hotelb + "1_south" } );
                nearby.push_back( { 0, hotelb + "1", 1, hotelb + "2", hotelb + "1_west" } );
            } else if( old == hotelb + "2" ) {
                nearby.push_back( { -1, hotelb + "3", 0, hotelb + "2", hotelb + "2_north" } );
                nearby.push_back( { 0, hotelb + "2", -1, hotelb + "3", hotelb + "2_east" } );
                nearby.push_back( { 1, hotelb + "3", 0, hotelb + "2", hotelb + "2_south" } );
                nearby.push_back( { 0, hotelb + "2", 1, hotelb + "3", hotelb + "2_west" } );
            } else if( old == hotelb + "3" ) {
                nearby.push_back( { 1, hotelb + "2", 0, hotelb + "3", hotelb + "3_north" } );
                nearby.push_back( { 0, hotelb + "3", 1, hotelb + "2", hotelb + "3_east" } );
                nearby.push_back( { -1, hotelb + "2", 0, hotelb + "3", hotelb + "3_south" } );
                nearby.push_back( { 0, hotelb + "3", -1, hotelb + "2", hotelb + "3_west" } );
            }
        } else if( old == "bunker" ) {
            if( pos.z < 0 ) {
                new_id = oter_id( "bunker_basement" );
            } else if( is_ot_match( "road", get_ter( pos.x + 1, pos.y, pos.z ), ot_match_type::TYPE ) ) {
                new_id = oter_id( "bunker_west" );
            } else if( is_ot_match( "road", get_ter( pos.x - 1, pos.y, pos.z ), ot_match_type::TYPE ) ) {
                new_id = oter_id( "bunker_east" );
            } else if( is_ot_match( "road", get_ter( pos.x, pos.y + 1, pos.z ), ot_match_type::TYPE ) ) {
                new_id = oter_id( "bunker_north" );
            } else {
                new_id = oter_id( "bunker_south" );
            }

        } else if( old == "farm" ) {
            new_id = oter_id( "farm_2_north" );

        } else if( old == "farm_field" ) {
            nearby.push_back( { -1, "farm",        1, "farm_field", "farm_1_north" } );
            nearby.push_back( {  1, "farm",        1, "farm_field", "farm_3_north" } );
            nearby.push_back( { -2, "farm_field",  1, "farm_field", "farm_4_north" } );
            nearby.push_back( {  0, "farm_field", -1, "farm",       "farm_5_north" } );
            nearby.push_back( {  2, "farm_field",  1, "farm_field", "farm_6_north" } );
            nearby.push_back( { -2, "farm_field", -2, "farm_field", "farm_7_north" } );
            nearby.push_back( {  0, "farm_field", -2, "farm",       "farm_8_north" } );
            nearby.push_back( {  2, "farm_field", -2, "farm_field", "farm_9_north" } );
        } else if( old.compare( 0, 7, "mansion" ) == 0 ) {
            if( old == "mansion_entrance" ) {
                new_id = oter_id( "mansion_e1_north" );
            } else if( old == "mansion" ) {
                nearby.push_back( { -1, "mansion_entrance",  1, "mansion",          "mansion_c1_east" } );
                nearby.push_back( {  1, "mansion_entrance",  1, "mansion",          "mansion_c3_north" } );
                nearby.push_back( { -2, "mansion",           1, "mansion",          "mansion_t2_west" } );
                nearby.push_back( {  0, "mansion",          -1, "mansion_entrance", "mansion_+4_north" } );
                nearby.push_back( {  2, "mansion",           1, "mansion",          "mansion_t4_east" } );
                nearby.push_back( { -2, "mansion",          -2, "mansion",          "mansion_c4_south" } );
                nearby.push_back( {  0, "mansion",          -2, "mansion_entrance", "mansion_t2_north" } );
                nearby.push_back( {  2, "mansion",          -2, "mansion",          "mansion_c2_west" } );
            }

            // Migrate terrains with NO_ROTATE flag to rotatable
        } else if( old.compare( 0, 4, "lmoe" ) == 0 ||
                   old.compare( 0, 5, "cabin" ) == 0 ||
                   old.compare( 0, 5, "pond_" ) == 0 ||
                   old.compare( 0, 6, "bandit" ) == 0 ||
                   old.compare( 0, 7, "haz_sar" ) == 0 ||
                   old.compare( 0, 7, "shelter" ) == 0 ||
                   old.compare( 0, 8, "campsite" ) == 0 ||
                   old.compare( 0, 9, "pwr_large" ) == 0 ||
                   old.compare( 0, 9, "shipwreck" ) == 0 ||
                   old.compare( 0, 9, "robofachq" ) == 0 ||
                   old.compare( 0, 10, "ranch_camp" ) == 0 ||
                   old.compare( 0, 11, "hdwr_large_" ) == 0 ||
                   old.compare( 0, 14, "loffice_tower_" ) == 0 ||
                   old.compare( 0, 17, "cemetery_4square_" ) == 0 ) {
            new_id = oter_id( old + "_north" );

        } else if( old == "hunter_shack" ||
                   old == "outpost" ||
                   old == "park" ||
                   old == "pool" ||
                   old == "pwr_sub_s" ||
                   old == "radio_tower" ||
                   old == "sai" ||
                   old == "toxic_dump" ) {
            new_id = oter_id( old + "_north" );
        }

        for( const auto &conv : nearby ) {
            const auto x_it = needs_conversion.find( tripoint( pos.x + conv.xoffset, pos.y, pos.z ) );
            const auto y_it = needs_conversion.find( tripoint( pos.x, pos.y + conv.yoffset, pos.z ) );
            if( x_it != needs_conversion.end() && x_it->second == conv.x_id &&
                y_it != needs_conversion.end() && y_it->second == conv.y_id ) {
                new_id = oter_id( conv.new_id );
                break;
            }
        }
    }
}

void overmap::load_monster_groups( JsonIn &jsin )
{
    jsin.start_array();
    while( !jsin.end_array() ) {
        jsin.start_array();

        mongroup new_group;
        new_group.deserialize( jsin );

        jsin.start_array();
        tripoint temp;
        while( !jsin.end_array() ) {
            temp.deserialize( jsin );
            new_group.pos = temp;
            add_mon_group( new_group );
        }

        jsin.end_array();
    }
}

void overmap::load_legacy_monstergroups( JsonIn &jsin )
{
    jsin.start_array();
    while( !jsin.end_array() ) {
        mongroup new_group;
        new_group.deserialize_legacy( jsin );
        add_mon_group( new_group );
    }
}

// throws std::exception
void overmap::unserialize( std::istream &fin )
{

    if( fin.peek() == '#' ) {
        // This was the last savegame version that produced the old format.
        static int overmap_legacy_save_version = 24;
        std::string vline;
        getline( fin, vline );
        std::string tmphash;
        std::string tmpver;
        int savedver = -1;
        std::stringstream vliness( vline );
        vliness >> tmphash >> tmpver >> savedver;
        if( savedver <= overmap_legacy_save_version ) {
            unserialize_legacy( fin );
            return;
        }
    }

    JsonIn jsin( fin );
    jsin.start_object();
    while( !jsin.end_object() ) {
        const std::string name = jsin.get_member_name();
        if( name == "layers" ) {
            std::unordered_map<tripoint, std::string> needs_conversion;
            jsin.start_array();
            for( int z = 0; z < OVERMAP_LAYERS; ++z ) {
                jsin.start_array();
                int count = 0;
                std::string tmp_ter;
                oter_id tmp_otid( 0 );
                for( int j = 0; j < OMAPY; j++ ) {
                    for( int i = 0; i < OMAPX; i++ ) {
                        if( count == 0 ) {
                            jsin.start_array();
                            jsin.read( tmp_ter );
                            jsin.read( count );
                            jsin.end_array();
                            if( obsolete_terrain( tmp_ter ) ) {
                                for( int p = i; p < i + count; p++ ) {
                                    needs_conversion.emplace( tripoint( p, j, z - OVERMAP_DEPTH ),
                                                              tmp_ter );
                                }
                                tmp_otid = oter_id( 0 );
                            } else if( oter_str_id( tmp_ter ).is_valid() ) {
                                tmp_otid = oter_id( tmp_ter );
                            } else {
                                debugmsg( "Loaded bad ter! ter %s", tmp_ter.c_str() );
                                tmp_otid = oter_id( 0 );
                            }
                        }
                        count--;
                        layer[z].terrain[i][j] = tmp_otid;
                    }
                }
                jsin.end_array();
            }
            jsin.end_array();
            convert_terrain( needs_conversion );
        } else if( name == "region_id" ) {
            std::string new_region_id;
            jsin.read( new_region_id );
            if( settings.id != new_region_id ) {
                t_regional_settings_map_citr rit = region_settings_map.find( new_region_id );
                if( rit != region_settings_map.end() ) {
                    settings = rit->second; // TODO: optimize
                }
            }
        } else if( name == "mongroups" ) {
            load_legacy_monstergroups( jsin );
        } else if( name == "monster_groups" ) {
            load_monster_groups( jsin );
        } else if( name == "cities" ) {
            jsin.start_array();
            while( !jsin.end_array() ) {
                jsin.start_object();
                city new_city;
                while( !jsin.end_object() ) {
                    std::string city_member_name = jsin.get_member_name();
                    if( city_member_name == "name" ) {
                        jsin.read( new_city.name );
                    } else if( city_member_name == "x" ) {
                        jsin.read( new_city.pos.x );
                    } else if( city_member_name == "y" ) {
                        jsin.read( new_city.pos.y );
                    } else if( city_member_name == "size" ) {
                        jsin.read( new_city.size );
                    }
                }
                cities.push_back( new_city );
            }
        } else if( name == "roads_out" ) {
            jsin.start_array();
            while( !jsin.end_array() ) {
                jsin.start_object();
                city new_road;
                while( !jsin.end_object() ) {
                    std::string road_member_name = jsin.get_member_name();
                    if( road_member_name == "x" ) {
                        jsin.read( new_road.pos.x );
                    } else if( road_member_name == "y" ) {
                        jsin.read( new_road.pos.y );
                    }
                }
                roads_out.push_back( new_road );
            }
        } else if( name == "radios" ) {
            jsin.start_array();
            while( !jsin.end_array() ) {
                jsin.start_object();
                radio_tower new_radio;
                while( !jsin.end_object() ) {
                    const std::string radio_member_name = jsin.get_member_name();
                    if( radio_member_name == "type" ) {
                        const std::string radio_name = jsin.get_string();
                        const auto mapping =
                            find_if( radio_type_names.begin(), radio_type_names.end(),
                        [radio_name]( const std::pair<int, std::string> &p ) {
                            return p.second == radio_name;
                        } );
                        if( mapping != radio_type_names.end() ) {
                            new_radio.type = mapping->first;
                        }
                    } else if( radio_member_name == "x" ) {
                        jsin.read( new_radio.x );
                    } else if( radio_member_name == "y" ) {
                        jsin.read( new_radio.y );
                    } else if( radio_member_name == "strength" ) {
                        jsin.read( new_radio.strength );
                    } else if( radio_member_name == "message" ) {
                        jsin.read( new_radio.message );
                    }
                }
                radios.push_back( new_radio );
            }
        } else if( name == "monster_map" ) {
            jsin.start_array();
            while( !jsin.end_array() ) {
                tripoint monster_location;
                monster new_monster;
                monster_location.deserialize( jsin );
                new_monster.deserialize( jsin );
                monster_map.insert( std::make_pair( std::move( monster_location ),
                                                    std::move( new_monster ) ) );
            }
        } else if( name == "tracked_vehicles" ) {
            jsin.start_array();
            while( !jsin.end_array() ) {
                jsin.start_object();
                om_vehicle new_tracker;
                int id;
                while( !jsin.end_object() ) {
                    std::string tracker_member_name = jsin.get_member_name();
                    if( tracker_member_name == "id" ) {
                        jsin.read( id );
                    } else if( tracker_member_name == "x" ) {
                        jsin.read( new_tracker.x );
                    } else if( tracker_member_name == "y" ) {
                        jsin.read( new_tracker.y );
                    } else if( tracker_member_name == "name" ) {
                        jsin.read( new_tracker.name );
                    }
                }
                vehicles[id] = new_tracker;
            }
        } else if( name == "scent_traces" ) {
            jsin.start_array();
            while( !jsin.end_array() ) {
                jsin.start_object();
                tripoint pos;
                time_point time = calendar::before_time_starts;
                int strength = 0;
                while( !jsin.end_object() ) {
                    std::string scent_member_name = jsin.get_member_name();
                    if( scent_member_name == "pos" ) {
                        jsin.read( pos );
                    } else if( scent_member_name == "time" ) {
                        jsin.read( time );
                    } else if( scent_member_name == "strength" ) {
                        jsin.read( strength );
                    }
                }
                scents[pos] = scent_trace( time, strength );
            }
        } else if( name == "npcs" ) {
            jsin.start_array();
            while( !jsin.end_array() ) {
                std::shared_ptr<npc> new_npc = std::make_shared<npc>();
                new_npc->deserialize( jsin );
                if( !new_npc->fac_id.str().empty() ) {
                    new_npc->set_fac( new_npc->fac_id );
                }
                npcs.push_back( new_npc );
            }
        } else if( name == "camps" ) {
            jsin.start_array();
            while( !jsin.end_array() ) {
                basecamp new_camp;
                new_camp.deserialize( jsin );
                camps.push_back( new_camp );
            }
        } else if( name == "overmap_special_placements" ) {
            jsin.start_array();
            while( !jsin.end_array() ) {
                jsin.start_object();
                overmap_special_id s;
                while( !jsin.end_object() ) {
                    std::string name = jsin.get_member_name();
                    if( name == "special" ) {
                        jsin.read( s );
                    } else if( name == "placements" ) {
                        jsin.start_array();
                        while( !jsin.end_array() ) {
                            jsin.start_object();
                            while( !jsin.end_object() ) {
                                std::string name = jsin.get_member_name();
                                if( name == "points" ) {
                                    jsin.start_array();
                                    while( !jsin.end_array() ) {
                                        jsin.start_object();
                                        tripoint p;
                                        while( !jsin.end_object() ) {
                                            std::string name = jsin.get_member_name();
                                            if( name == "p" ) {
                                                jsin.read( p );
                                                overmap_special_placements[p] = s;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

static void unserialize_array_from_compacted_sequence( JsonIn &jsin, bool ( &array )[OMAPX][OMAPY] )
{
    int count = 0;
    bool value = false;
    for( int j = 0; j < OMAPY; j++ ) {
        for( int i = 0; i < OMAPX; i++ ) {
            if( count == 0 ) {
                jsin.start_array();
                jsin.read( value );
                jsin.read( count );
                jsin.end_array();
            }
            count--;
            array[i][j] = value;
        }
    }
}

// throws std::exception
void overmap::unserialize_view( std::istream &fin )
{
    // Private/per-character view of the overmap.
    if( fin.peek() == '#' ) {
        // This was the last savegame version that produced the old format.
        static int overmap_legacy_save_version = 24;
        std::string vline;
        getline( fin, vline );
        std::string tmphash;
        std::string tmpver;
        int savedver = -1;
        std::stringstream vliness( vline );
        vliness >> tmphash >> tmpver >> savedver;
        if( savedver <= overmap_legacy_save_version ) {
            unserialize_view_legacy( fin );
            return;
        }
    }

    JsonIn jsin( fin );
    jsin.start_object();
    while( !jsin.end_object() ) {
        const std::string name = jsin.get_member_name();
        if( name == "visible" ) {
            jsin.start_array();
            for( int z = 0; z < OVERMAP_LAYERS; ++z ) {
                jsin.start_array();
                unserialize_array_from_compacted_sequence( jsin, layer[z].visible );
                jsin.end_array();
            }
            jsin.end_array();
        } else if( name == "explored" ) {
            jsin.start_array();
            for( int z = 0; z < OVERMAP_LAYERS; ++z ) {
                jsin.start_array();
                unserialize_array_from_compacted_sequence( jsin, layer[z].explored );
                jsin.end_array();
            }
            jsin.end_array();
        } else if( name == "notes" ) {
            jsin.start_array();
            for( int z = 0; z < OVERMAP_LAYERS; ++z ) {
                jsin.start_array();
                while( !jsin.end_array() ) {
                    om_note tmp;
                    jsin.start_array();
                    jsin.read( tmp.x );
                    jsin.read( tmp.y );
                    jsin.read( tmp.text );
                    jsin.end_array();

                    layer[z].notes.push_back( tmp );
                }
            }
            jsin.end_array();
        } else if( name == "extras" ) {
            jsin.start_array();
            for( int z = 0; z < OVERMAP_LAYERS; ++z ) {
                jsin.start_array();
                while( !jsin.end_array() ) {
                    om_map_extra tmp;
                    jsin.start_array();
                    jsin.read( tmp.x );
                    jsin.read( tmp.y );
                    jsin.read( tmp.id );
                    jsin.end_array();

                    layer[z].extras.push_back( tmp );
                }
            }
            jsin.end_array();
        }
    }
}

static void serialize_array_to_compacted_sequence( JsonOut &json,
        const bool ( &array )[OMAPX][OMAPY] )
{
    int count = 0;
    int lastval = -1;
    for( int j = 0; j < OMAPY; j++ ) {
        for( int i = 0; i < OMAPX; i++ ) {
            const int value = array[i][j];
            if( value != lastval ) {
                if( count ) {
                    json.write( count );
                    json.end_array();
                }
                lastval = value;
                json.start_array();
                json.write( static_cast<bool>( value ) );
                count = 1;
            } else {
                count++;
            }
        }
    }
    json.write( count );
    json.end_array();
}

void overmap::serialize_view( std::ostream &fout ) const
{
    static const int first_overmap_view_json_version = 25;
    fout << "# version " << first_overmap_view_json_version << std::endl;

    JsonOut json( fout, false );
    json.start_object();

    json.member( "visible" );
    json.start_array();
    for( int z = 0; z < OVERMAP_LAYERS; ++z ) {
        json.start_array();
        serialize_array_to_compacted_sequence( json, layer[z].visible );
        json.end_array();
        fout << std::endl;
    }
    json.end_array();

    json.member( "explored" );
    json.start_array();
    for( int z = 0; z < OVERMAP_LAYERS; ++z ) {
        json.start_array();
        serialize_array_to_compacted_sequence( json, layer[z].explored );
        json.end_array();
        fout << std::endl;
    }
    json.end_array();

    json.member( "notes" );
    json.start_array();
    for( int z = 0; z < OVERMAP_LAYERS; ++z ) {
        json.start_array();
        for( auto &i : layer[z].notes ) {
            json.start_array();
            json.write( i.x );
            json.write( i.y );
            json.write( i.text );
            json.end_array();
            fout << std::endl;
        }
        json.end_array();
    }
    json.end_array();

    json.member( "extras" );
    json.start_array();
    for( int z = 0; z < OVERMAP_LAYERS; ++z ) {
        json.start_array();
        for( auto &i : layer[z].extras ) {
            json.start_array();
            json.write( i.x );
            json.write( i.y );
            json.write( i.id );
            json.end_array();
            fout << std::endl;
        }
        json.end_array();
    }
    json.end_array();

    json.end_object();
}

// Compares all fields except position and monsters
// If any group has monsters, it is never equal to any group (because monsters are unique)
struct mongroup_bin_eq {
    bool operator()( const mongroup &a, const mongroup &b ) const {
        return a.monsters.empty() &&
               b.monsters.empty() &&
               a.type == b.type &&
               a.radius == b.radius &&
               a.population == b.population &&
               a.target == b.target &&
               a.interest == b.interest &&
               a.dying == b.dying &&
               a.horde == b.horde &&
               a.horde_behaviour == b.horde_behaviour &&
               a.diffuse == b.diffuse;
    }
};

struct mongroup_hash {
    std::size_t operator()( const mongroup &mg ) const {
        // Note: not hashing monsters or position
        size_t ret = std::hash<mongroup_id>()( mg.type );
        std::hash_combine( ret, mg.radius );
        std::hash_combine( ret, mg.population );
        std::hash_combine( ret, mg.target );
        std::hash_combine( ret, mg.interest );
        std::hash_combine( ret, mg.dying );
        std::hash_combine( ret, mg.horde );
        std::hash_combine( ret, mg.horde_behaviour );
        std::hash_combine( ret, mg.diffuse );
        return ret;
    }
};

void overmap::save_monster_groups( JsonOut &jout ) const
{
    jout.member( "monster_groups" );
    jout.start_array();
    // Bin groups by their fields, except positions and monsters
    std::unordered_map<mongroup, std::list<tripoint>, mongroup_hash, mongroup_bin_eq> binned_groups;
    binned_groups.reserve( zg.size() );
    for( const auto &pos_group : zg ) {
        // Each group in bin adds only position
        // so that 100 identical groups are 1 group data and 100 tripoints
        std::list<tripoint> &positions = binned_groups[pos_group.second];
        positions.emplace_back( pos_group.first );
    }

    for( auto &group_bin : binned_groups ) {
        jout.start_array();
        // Zero the bin position so that it isn't serialized
        // The position is stored separately, in the list
        // TODO: Do it without the copy
        mongroup saved_group = group_bin.first;
        saved_group.pos = tripoint_zero;
        jout.write( saved_group );
        jout.write( group_bin.second );
        jout.end_array();
    }
    jout.end_array();
}

void overmap::serialize( std::ostream &fout ) const
{
    static const int first_overmap_json_version = 26;
    fout << "# version " << first_overmap_json_version << std::endl;

    JsonOut json( fout, false );
    json.start_object();

    json.member( "layers" );
    json.start_array();
    for( int z = 0; z < OVERMAP_LAYERS; ++z ) {
        int count = 0;
        oter_id last_tertype( -1 );
        json.start_array();
        for( int j = 0; j < OMAPY; j++ ) {
            for( int i = 0; i < OMAPX; i++ ) {
                oter_id t = layer[z].terrain[i][j];
                if( t != last_tertype ) {
                    if( count ) {
                        json.write( count );
                        json.end_array();
                    }
                    last_tertype = t;
                    json.start_array();
                    json.write( t.id() );
                    count = 1;
                } else {
                    count++;
                }
            }
        }
        json.write( count );
        // End the last entry for a z-level.
        json.end_array();
        // End the z-level
        json.end_array();
        // Insert a newline occasionally so the file isn't totally unreadable.
        fout << std::endl;
    }
    json.end_array();

    // temporary, to allow user to manually switch regions during play until regionmap is done.
    json.member( "region_id", settings.id );
    fout << std::endl;

    save_monster_groups( json );
    fout << std::endl;

    json.member( "cities" );
    json.start_array();
    for( auto &i : cities ) {
        json.start_object();
        json.member( "name", i.name );
        json.member( "x", i.pos.x );
        json.member( "y", i.pos.y );
        json.member( "size", i.size );
        json.end_object();
    }
    json.end_array();
    fout << std::endl;

    json.member( "roads_out" );
    json.start_array();
    for( auto &i : roads_out ) {
        json.start_object();
        json.member( "x", i.pos.x );
        json.member( "y", i.pos.y );
        json.end_object();
    }
    json.end_array();
    fout << std::endl;

    json.member( "radios" );
    json.start_array();
    for( auto &i : radios ) {
        json.start_object();
        json.member( "x", i.x );
        json.member( "y", i.y );
        json.member( "strength", i.strength );
        json.member( "type", radio_type_names[i.type] );
        json.member( "message", i.message );
        json.end_object();
    }
    json.end_array();
    fout << std::endl;

    json.member( "monster_map" );
    json.start_array();
    for( auto &i : monster_map ) {
        i.first.serialize( json );
        i.second.serialize( json );
    }
    json.end_array();
    fout << std::endl;

    json.member( "tracked_vehicles" );
    json.start_array();
    for( const auto &i : vehicles ) {
        json.start_object();
        json.member( "id", i.first );
        json.member( "name", i.second.name );
        json.member( "x", i.second.x );
        json.member( "y", i.second.y );
        json.end_object();
    }
    json.end_array();
    fout << std::endl;

    json.member( "scent_traces" );
    json.start_array();
    for( const auto &scent : scents ) {
        json.start_object();
        json.member( "pos", scent.first );
        json.member( "time", scent.second.creation_time );
        json.member( "strength", scent.second.initial_strength );
        json.end_object();
    }
    json.end_array();
    fout << std::endl;

    json.member( "npcs" );
    json.start_array();
    for( auto &i : npcs ) {
        json.write( *i );
    }
    json.end_array();
    fout << std::endl;

    json.member( "camps" );
    json.start_array();
    for( auto &i : camps ) {
        json.write( i );
    }
    json.end_array();
    fout << std::endl;

    // Condense the overmap special placements so that all placements of a given special
    // are grouped under a single key for that special.
    std::map<overmap_special_id, std::vector<tripoint>> condensed_overmap_special_placements;
    for( const auto &placement : overmap_special_placements ) {
        condensed_overmap_special_placements[placement.second].emplace_back( placement.first );
    }

    json.member( "overmap_special_placements" );
    json.start_array();
    for( const auto &placement : condensed_overmap_special_placements ) {
        json.start_object();
        json.member( "special", placement.first );
        json.member( "placements" );
        json.start_array();
        // When we have a discriminator for different instances of a given special,
        // we'd use that that group them, but since that doesn't exist yet we'll
        // dump all the points of a given special into a single entry.
        json.start_object();
        json.member( "points" );
        json.start_array();
        for( const tripoint &pos : placement.second ) {
            json.start_object();
            json.member( "p", pos );
            json.end_object();
        }
        json.end_array();
        json.end_object();
        json.end_array();
        json.end_object();
    }
    json.end_array();
    fout << std::endl;

    json.end_object();
    fout << std::endl;
}

////////////////////////////////////////////////////////////////////////////////////////
///// mongroup
template<typename Archive>
void mongroup::io( Archive &archive )
{
    archive.io( "type", type );
    archive.io( "pos", pos, tripoint_zero );
    archive.io( "radius", radius, 1u );
    archive.io( "population", population, 1u );
    archive.io( "diffuse", diffuse, false );
    archive.io( "dying", dying, false );
    archive.io( "horde", horde, false );
    archive.io( "target", target, tripoint_zero );
    archive.io( "interest", interest, 0 );
    archive.io( "horde_behaviour", horde_behaviour, io::empty_default_tag() );
    archive.io( "monsters", monsters, io::empty_default_tag() );
}

void mongroup::deserialize( JsonIn &data )
{
    io::JsonObjectInputArchive archive( data );
    io( archive );
}

void mongroup::serialize( JsonOut &json ) const
{
    io::JsonObjectOutputArchive archive( json );
    const_cast<mongroup *>( this )->io( archive );
}

void mongroup::deserialize_legacy( JsonIn &json )
{
    json.start_object();
    while( !json.end_object() ) {
        std::string name = json.get_member_name();
        if( name == "type" ) {
            type = mongroup_id( json.get_string() );
        } else if( name == "pos" ) {
            pos.deserialize( json );
        } else if( name == "radius" ) {
            radius = json.get_int();
        } else if( name == "population" ) {
            population = json.get_int();
        } else if( name == "diffuse" ) {
            diffuse = json.get_bool();
        } else if( name == "dying" ) {
            dying = json.get_bool();
        } else if( name == "horde" ) {
            horde = json.get_bool();
        } else if( name == "target" ) {
            target.deserialize( json );
        } else if( name == "interest" ) {
            interest = json.get_int();
        } else if( name == "horde_behaviour" ) {
            horde_behaviour = json.get_string();
        } else if( name == "monsters" ) {
            json.start_array();
            while( !json.end_array() ) {
                monster new_monster;
                new_monster.deserialize( json );
                monsters.push_back( new_monster );
            }
        }
    }
}

////////////////////////////////////////////////////////////////////////////////////////
///// mapbuffer

///////////////////////////////////////////////////////////////////////////////////////
///// master.gsav

void mission::unserialize_all( JsonIn &jsin )
{
    jsin.start_array();
    while( !jsin.end_array() ) {
        mission mis;
        mis.deserialize( jsin );
        add_existing( mis );
    }
}

void game::unserialize_master( std::istream &fin )
{
    savegame_loading_version = 0;
    chkversion( fin );
    if( savegame_loading_version != savegame_version && savegame_loading_version < 11 ) {
        popup_nowait(
            _( "Cannot find loader for save data in old version %d, attempting to load as current version %d." ),
            savegame_loading_version, savegame_version );
    }
    try {
        // single-pass parsing example
        JsonIn jsin( fin );
        jsin.start_object();
        while( !jsin.end_object() ) {
            std::string name = jsin.get_member_name();
            if( name == "next_mission_id" ) {
                next_mission_id = jsin.get_int();
            } else if( name == "next_npc_id" ) {
                next_npc_id = jsin.get_int();
            } else if( name == "active_missions" ) {
                mission::unserialize_all( jsin );
            } else if( name == "factions" ) {
                jsin.read( *faction_manager_ptr );
            } else {
                // silently ignore anything else
                jsin.skip_value();
            }
        }
    } catch( const JsonError &e ) {
        debugmsg( "error loading master.gsav: %s", e.c_str() );
    }
}

void mission::serialize_all( JsonOut &json )
{
    json.start_array();
    for( auto &e : get_all_active() ) {
        e->serialize( json );
    }
    json.end_array();
}

void game::serialize_master( std::ostream &fout )
{
    fout << "# version " << savegame_version << std::endl;
    try {
        JsonOut json( fout, true ); // pretty-print
        json.start_object();

        json.member( "next_mission_id", next_mission_id );
        json.member( "next_npc_id", next_npc_id );

        json.member( "active_missions" );
        mission::serialize_all( json );

        json.member( "factions", *faction_manager_ptr );

        json.end_object();
    } catch( const JsonError &e ) {
        debugmsg( "error saving to master.gsav: %s", e.c_str() );
    }
}

void faction_manager::serialize( JsonOut &jsout ) const
{
    jsout.write( factions );
}

void faction_manager::deserialize( JsonIn &jsin )
{
    jsin.start_array();
    while( !jsin.end_array() ) {
        faction add_fac;
        jsin.read( add_fac );
        faction *old_fac = get( add_fac.id );
        if( old_fac ) {
            *old_fac = add_fac;
            // force a revalidation of add_fac
            get( add_fac.id );
        } else {
            factions.emplace_back( add_fac );
        }
    }
}

void Creature_tracker::deserialize( JsonIn &jsin )
{
    monsters_list.clear();
    monsters_by_location.clear();
    jsin.start_array();
    while( !jsin.end_array() ) {
        monster montmp;
        jsin.read( montmp );
        add( montmp );
    }
}

void Creature_tracker::serialize( JsonOut &jsout ) const
{
    jsout.start_array();
    for( const auto &monster_ptr : monsters_list ) {
        jsout.write( *monster_ptr );
    }
    jsout.end_array();
}
