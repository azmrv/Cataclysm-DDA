#pragma once
#ifndef MAGIC_H
#define MAGIC_H

#include <map>
#include <set>

#include "bodypart.h"
#include "damage.h"
#include "enum_bitset.h"
#include "type_id.h"
#include "ui.h"

struct mutation_branch;
struct tripoint;
struct dealt_damage_instance;
struct damage_instance;

class player;
class JsonObject;
class JsonOut;
class JsonIn;
class teleporter_list;
class time_duration;
class nc_color;

enum spell_flag {
    PERMANENT, // items or creatures spawned with this spell do not disappear and die as normal
    IGNORE_WALLS, // spell's aoe goes through walls
    HOSTILE_SUMMON, // summon spell always spawns a hostile monster
    HOSTILE_50, // summoned monster spawns friendly 50% of the time
    SILENT, // spell makes no noise at target
    LOUD, // spell makes extra noise at target
    VERBAL, // spell makes noise at caster location, mouth encumbrance affects fail %
    SOMATIC, // arm encumbrance affects fail % and casting time (slightly)
    NO_HANDS, // hands do not affect spell energy cost
    NO_LEGS, // legs do not affect casting time
    CONCENTRATE, // focus affects spell fail %
    LAST
};

enum energy_type {
    hp_energy,
    mana_energy,
    stamina_energy,
    bionic_energy,
    fatigue_energy,
    none_energy
};

enum valid_target {
    target_ally,
    target_hostile,
    target_self,
    target_ground,
    target_none,
    _LAST
};

template<>
struct enum_traits<valid_target> {
    static constexpr auto last = valid_target::_LAST;
};

template<>
struct enum_traits<spell_flag> {
    static constexpr auto last = spell_flag::LAST;
};

class spell_type
{
    public:

        spell_type() = default;

        bool was_loaded = false;

        spell_id id;
        // spell name
        std::string name;
        // spell description
        std::string description;
        // spell effect string. used to look up spell function
        std::string effect;
        // extra information about spell effect. allows for combinations for effects
        std::string effect_str;

        // minimum damage this spell can cause
        int min_damage;
        // amount of damage increase per spell level
        float damage_increment;
        // maximum damage this spell can cause
        int max_damage;

        // minimum range of a spell
        int min_range;
        // amount of range increase per spell level
        float range_increment;
        // max range this spell can achieve
        int max_range;

        // minimum area of effect of a spell (radius)
        // 0 means the spell only affects the target
        int min_aoe;
        // amount of area of effect increase per spell level (radius)
        float aoe_increment;
        // max area of effect of a spell (radius)
        int max_aoe;

        // damage over time deals damage per turn

        // minimum damage over time
        int min_dot;
        // increment per spell level
        float dot_increment;
        // max damage over time
        int max_dot;

        // amount of time effect lasts

        // minimum time for effect in moves
        int min_duration;
        // increment per spell level in moves
        // DoT is per turn, but increments can be smaller
        int duration_increment;
        // max time for effect in moves
        int max_duration;

        // amount of damage that is piercing damage
        // not added to damage stat

        // minimum pierce damage
        int min_pierce;
        // increment of pierce damage per spell level
        float pierce_increment;
        // max pierce damage
        int max_pierce;

        // base energy cost of spell
        int base_energy_cost;
        // increment of energy cost per spell level
        float energy_increment;
        // max or min energy cost, based on sign of energy_increment
        int final_energy_cost;

        // spell is restricted to being cast by only this class
        // if spell_class is empty, spell is unrestricted
        trait_id spell_class;

        // the difficulty of casting a spell
        int difficulty;

        // max level this spell can achieve
        int max_level;

        // base amount of time to cast the spell in moves
        int base_casting_time;
        // increment of casting time per level
        float casting_time_increment;
        // max or min casting time
        int final_casting_time;

        // what energy do you use to cast this spell
        energy_type energy_source;

        damage_type dmg_type;

        // list of valid targets enum
        enum_bitset<valid_target> valid_targets;

        // lits of bodyparts this spell applies its effect to
        enum_bitset<body_part> affected_bps;

        enum_bitset<spell_flag> spell_tags;

        static void load_spell( JsonObject &jo, const std::string &src );
        void load( JsonObject &jo, const std::string & );
        /**
         * All spells in the game.
         */
        static const std::vector<spell_type> &get_all();
        static void check_consistency();
        static void reset_all();
        bool is_valid() const;
};

class spell
{
    private:
        // basic spell data
        const spell_type *type;

        // once you accumulate enough exp you level the spell
        int experience;
        // returns damage type for the spell
        damage_type dmg_type() const;

    public:
        spell() = default;
        spell( const spell_type *sp, int xp = 0 );
        spell( spell_id sp, int xp = 0 );

        // how much exp you need for the spell to gain a level
        int exp_to_next_level() const;
        // progress to the next level, expressed as a percent
        std::string exp_progress() const;
        // how much xp you have total
        int xp() const;
        // gain some exp
        void gain_exp( int nxp );
        // how much xp you get if you successfully cast the spell
        int casting_exp( const player &p ) const;
        // modifier for gaining exp
        float exp_modifier( const player &p ) const;
        // level up!
        void gain_level();
        // is the spell at max level?
        bool is_max_level() const;
        // what is the max level of the spell
        int get_max_level() const;

        // how much damage does the spell do
        int damage() const;
        dealt_damage_instance get_dealt_damage_instance() const;
        damage_instance get_damage_instance() const;
        // how big is the spell's radius
        int aoe() const;
        // distance spell can be cast
        int range() const;
        // how much energy does the spell cost
        int energy_cost( const player &p ) const;
        // how long does this spell's effect last
        int duration() const;
        time_duration duration_turns() const;
        // how often does the spell fail
        // based on difficulty, level of spell, spellcraft skill, intelligence
        float spell_fail( const player &p ) const;
        std::string colorized_fail_percent( const player &p ) const;
        // how long does it take to cast the spell
        int casting_time( const player &p ) const;

        // can the player cast this spell?
        bool can_cast( const player &p ) const;
        // can the player learn this spell?
        bool can_learn( const player &p ) const;
        // is this spell valid
        bool is_valid() const;
        // is the bodypart affected by the effect
        bool bp_is_affected( body_part bp ) const;
        // check if the spell has a particular flag
        bool has_flag( const spell_flag &flag ) const;
        // check if the spell's class is the same as input
        bool is_spell_class( const trait_id &mid ) const;

        // get spell id (from type)
        spell_id id() const;
        // get spell class (from type)
        trait_id spell_class() const;
        // get spell effect string (from type)
        std::string effect() const;
        // get spell effect_str data
        std::string effect_data() const;
        // name of spell (translated)
        std::string name() const;
        // description of spell (translated)
        std::string description() const;
        // energy source as a string (translated)
        std::string energy_string() const;
        // energy cost returned as a string
        std::string energy_cost_string( const player &p ) const;
        // current energy the player has available as a string
        std::string energy_cur_string( const player &p ) const;
        // prints out a list of valid targets separated by commas
        std::string enumerate_targets() const;
        // energy source enum
        energy_type energy_source() const;
        // the color that's representative of the damage type
        nc_color damage_type_color() const;
        std::string damage_type_string() const;
        // your level in this spell
        int get_level() const;
        // difficulty of the level
        int get_difficulty() const;

        // makes a spell sound at the location
        void make_sound( const tripoint &target ) const;
        // heals the critter at the location, returns amount healed (player heals each body part)
        int heal( const tripoint &target ) const;

        // is the target valid for this spell?
        bool is_valid_target( const tripoint &p ) const;
        bool is_valid_target( valid_target t ) const;
};

class known_magic
{
    private:
        // list of spells known
        std::map<spell_id, spell> spellbook;
        // invlets assigned to spell_id
        std::map<spell_id, int> invlets;
        // the base mana a player would start with
        int mana_base;
        // current mana
        int mana;
    public:
        // ignores all distractions when casting a spell when true
        bool casting_ignore = false;

        known_magic();

        void learn_spell( const std::string &sp, player &p, bool force = false );
        void learn_spell( const spell_id &sp, player &p, bool force = false );
        void learn_spell( const spell_type *sp, player &p, bool force = false );
        void forget_spell( const std::string &sp );
        void forget_spell( const spell_id &sp );
        // time in moves for the player to memorize the spell
        int time_to_learn_spell( const player &p, const spell_id &sp ) const;
        int time_to_learn_spell( const player &p, const std::string &str ) const;
        bool can_learn_spell( const player &p, const spell_id &sp ) const;
        bool knows_spell( const std::string &sp ) const;
        bool knows_spell( const spell_id &sp ) const;
        // spells known by player
        std::vector<spell_id> spells() const;
        // gets the spell associated with the spell_id to be edited
        spell &get_spell( const spell_id &sp );
        // opens up a ui that the player can choose a spell from
        // returns the index of the spell in the vector of spells
        int select_spell( const player &p );
        // get all known spells
        std::vector<spell *> get_spells();
        // how much mana is available to use to cast spells
        int available_mana() const;
        // max mana vailable
        int max_mana( const player &p ) const;
        void mod_mana( const player &p, int add_mana );
        void set_mana( int new_mana );
        void update_mana( const player &p, float turns );
        // does the player have enough energy to cast this spell?
        // not specific to mana
        bool has_enough_energy( const player &p, spell &sp ) const;

        void on_mutation_gain( const trait_id &mid, player &p );
        void on_mutation_loss( const trait_id &mid );

        void serialize( JsonOut &json ) const;
        void deserialize( JsonIn &jsin );
    private:
        // gets length of longest spell name
        size_t get_spellname_max_width();
        // gets invlet if assigned, or -1 if not
        int get_invlet( const spell_id &sp, std::set<int> &used_invlets );
};

namespace spell_effect
{
void teleport( int min_distance, int max_distance );
void pain_split(); // only does g->u
void move_earth( const tripoint &target );
void target_attack( const spell &sp, const tripoint &source, const tripoint &target );
void projectile_attack( const spell &sp, const tripoint &source, const tripoint &target );
void cone_attack( const spell &sp, const tripoint &source, const tripoint &target );
void line_attack( const spell &sp, const tripoint &source, const tripoint &target );

std::set<tripoint> spell_effect_blast( const spell &, const tripoint &, const tripoint &target,
                                       const int aoe_radius, const bool ignore_walls );
std::set<tripoint> spell_effect_cone( const spell &sp, const tripoint &source,
                                      const tripoint &target,
                                      const int aoe_radius, const bool ignore_walls );
std::set<tripoint> spell_effect_line( const spell &, const tripoint &source,
                                      const tripoint &target,
                                      const int aoe_radius, const bool ignore_walls );

void spawn_ethereal_item( spell &sp );
void recover_energy( spell &sp, const tripoint &target );
void spawn_summoned_monster( spell &sp, const tripoint &source, const tripoint &target );
void translocate( spell &sp, const tripoint &source, const tripoint &target,
                  teleporter_list &tp_list );
} // namespace spell_effect

class spellbook_callback : public uilist_callback
{
    private:
        std::vector<spell_type> spells;
    public:
        void add_spell( const spell_id &sp );
        void select( int entnum, uilist *menu ) override;
};

#endif
