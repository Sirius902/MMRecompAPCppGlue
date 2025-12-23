#include <filesystem>
#include <fstream>
#include <chrono>
#include <iomanip>
#include <algorithm>

#include "Archipelago.h"
#include "apcpp-glue.h"
#include "apcpp-solo-gen.h"

#define UPPER(v) ((((uint64_t) v) >> 32) & 0xFFFFFFFF)
#define LOWER(v) (((uint64_t) v) & 0xFFFFFFFF)

#define CRAFT_64(upper, lower) ((uint64_t) (((uint64_t) (((uint64_t) upper) << 32)) | ((uint64_t) lower)))

void glueGetLine(std::ifstream& in, std::string& outString)
{
    char c = in.get();
    
    while (c != '\r' && c != '\n' && c != '\0' && c != -1)
    {
        outString += c;
        c = in.get();
    }

    c = in.peek();

    while (c == '\r' || c == '\n')
    {
        in.get();
        c = in.peek();
    }
}

struct SoloSeed {
    std::u8string seed_name;
    std::filesystem::file_time_type timestamp;
    std::string date_string;
};

struct SoloState {
    std::vector<SoloSeed> seeds;
    std::filesystem::path seed_folder;
};

AP_State* state;
SoloState solo_state;
std::u8string room_seed_name;

constexpr std::u8string_view gen_file_prefix = u8"AP_";
constexpr std::u8string_view gen_file_suffix = u8"_solo.zip";

u32 hasItem(u64 itemId)
{
    u32 count = 0;
    u32 items_size = (u32) AP_GetReceivedItemsSize(state);
    for (u32 i = 0; i < items_size; ++i)
    {
        if (AP_GetReceivedItem(state, i) == itemId)
        {
            count += 1;
        }
    }
    return count;
}

int64_t fixLocation(u32 arg)
{
    if ((arg & 0xFF0000) == 0x090000 && AP_GetSlotDataInt(state, "shopsanity") == 1)
    {
        u32 shopItem = arg & 0xFFFF;
        switch (shopItem)
        {
            case SI_NUTS_2:
                shopItem = SI_NUTS_1;
                break;
            case SI_STICK_2:
                shopItem = SI_STICK_1;
                break;
            case SI_ARROWS_LARGE_2:
                shopItem = SI_ARROWS_LARGE_1;
                break;
            case SI_ARROWS_MEDIUM_2:
                shopItem = SI_ARROWS_MEDIUM_1;
                break;
            case SI_FAIRY_2:
                shopItem = SI_FAIRY_1;
                break;
            case SI_POTION_GREEN_3:
                shopItem = SI_POTION_GREEN_2;
                break;
            case SI_SHIELD_HERO_2:
                shopItem = SI_SHIELD_HERO_1;
                break;
            case SI_POTION_RED_3:
                shopItem = SI_POTION_RED_2;
                break;

            case SI_POTION_RED_6:
                shopItem = SI_POTION_RED_5;
                break;
            case SI_ARROWS_SMALL_3:
                shopItem = SI_ARROWS_SMALL_2;
                break;
            case SI_BOMB_3:
                shopItem = SI_BOMB_2;
                break;

            // case SI_BOTTLE:
            // case SI_SWORD_GREAT_FAIRY:
            // case SI_SWORD_KOKIRI:
            // case SI_SWORD_RAZOR:
            // case SI_SWORD_GILDED:
            //     shopItem = SI_SWORD_KOKIRI;
            //     break;
        }
        return 0x090000 | shopItem;
    }

    if (arg == 0x05481E && AP_GetSlotDataInt(state, "shopsanity") != 2) {
        return 0x054D1E;
    }
    return arg;
}

int64_t last_location_sent;

s16 prices[36];

void getStr(uint8_t* rdram, PTR(char) ptr, std::string& outString) {
    char c = MEM_B(0, (gpr) ptr);
    u32 i = 0;
    while (c != 0) {
        outString += c;
        i += 1;
        c = MEM_B(i, (gpr) ptr);
    }
}

void getU8Str(uint8_t* rdram, PTR(char) ptr, std::u8string& outString) {
    char8_t c = MEM_B(0, (gpr) ptr);
    u32 i = 0;
    while (c != 0) {
        outString += c;
        i += 1;
        c = MEM_B(i, (gpr) ptr);
    }
}

void setStr(uint8_t* rdram, PTR(char) ptr, const char* inString) {
    char c = -1;
    u32 i = 0;
    while (c != 0) {
        c = inString[i];
        MEM_B(i, (gpr) ptr) = c;
        i += 1;
    }
}

void setU8Str(uint8_t* rdram, PTR(u8) ptr, const char8_t* inString) {
    char8_t c = -1;
    u32 i = 0;
    while (c != 0) {
        c = inString[i];
        MEM_B(i, (gpr) ptr) = c;
        i += 1;
    }
}

template <typename TP>
std::time_t time_point_to_time_t(TP tp)
{
    // Approximation that uses two now calls to avoid relying on C++20 clock_cast. 
    auto sctp = time_point_cast<std::chrono::system_clock::duration>(tp - TP::clock::now()
              + std::chrono::system_clock::now());
    return std::chrono::system_clock::to_time_t(sctp);
}

std::string format_file_time(std::filesystem::file_time_type time) {
    std::time_t time_c = time_point_to_time_t(time);
    std::tm time_tm;
    bool success;

#if _WIN32
    success = localtime_s(&time_tm, &time_c) == 0;
#else
    success = localtime_r(&time_c, &time_tm) != nullptr;
#endif

    if (!success) {
        return "ERR";
    }

    std::stringstream sstream{};

    sstream << std::put_time(&time_tm, "%b %e %y %I:%M:%S %p");

    return sstream.str();
}

extern "C"
{
    DLLEXPORT u32 recomp_api_version = 1;

    bool rando_init_common() {
        AP_SetDeathLinkSupported(state, true);
        
        AP_Start(state);
        
        while (!AP_IsConnected(state))
        {
            if (AP_GetConnectionStatus(state) == AP_ConnectionStatus::ConnectionRefused || AP_GetConnectionStatus(state) == AP_ConnectionStatus::NotFound)
            {
                AP_Stop(state);
                return false;
            }
        }
        
        const char* prices_str = AP_GetSlotDataString(state, "shop_prices");
        
        std::stringstream prices_ss(prices_str);
        s16 price;
        size_t price_i = 0;
        
        while (prices_ss >> price)
        {
            prices[price_i] = price;
            price_i += 1;
        }
        
        AP_QueueLocationScoutsAll(state);
        
        if (AP_GetSlotDataInt(state, "skullsanity") == 2)
        {
            for (int i = 0x00; i <= 0x1E; ++i)
            {
                if (i == 0x03)
                {
                    continue;
                }
                
                int64_t location_id = 0x3469420062700 | i;
                AP_RemoveQueuedLocationScout(state, location_id);
            }
            for (int i = 0x01; i <= 0x1E; ++i)
            {
                int64_t location_id = 0x3469420062800 | i;
                AP_RemoveQueuedLocationScout(state, location_id);
            }
        }
        
        for (int64_t i = AP_GetSlotDataInt(state, "starting_heart_locations"); i < 8; ++i)
        {
            int64_t location_id = 0x34694200D0000 | i;
            AP_RemoveQueuedLocationScout(state, location_id);
        }

        if (AP_GetSlotDataInt(state, "cowsanity") == 0)
        {
            for (int i = 0x10; i <= 0x17; ++i)
            {
                int64_t location_id = 0x3469420BEEF00 | i;
                AP_RemoveQueuedLocationScout(state, location_id);
            }
        }
        
        if (AP_GetSlotDataInt(state, "scrubsanity") == 0)
        {
            AP_RemoveQueuedLocationScout(state, 0x3469420090100 | GI_MAGIC_BEANS);
            AP_RemoveQueuedLocationScout(state, 0x3469420090100 | GI_BOMB_BAG_40);
            AP_RemoveQueuedLocationScout(state, 0x3469420090100 | GI_POTION_GREEN);
            AP_RemoveQueuedLocationScout(state, 0x3469420090100 | GI_POTION_BLUE);
        }
        
        if (AP_GetSlotDataInt(state, "shopsanity") != 2)
        {
            AP_RemoveQueuedLocationScout(state, 0x346942005481E);
            AP_RemoveQueuedLocationScout(state, 0x3469420024234);
            
            if (AP_GetSlotDataInt(state, "shopsanity") == 1)
            {
                for (int i = SI_FAIRY_2; i <= SI_POTION_RED_3; ++i)
                {
                    int64_t location_id = 0x3469420090000 | i;
                    AP_RemoveQueuedLocationScout(state, location_id);
                }
                
                AP_RemoveQueuedLocationScout(state, 0x3469420090000 | SI_BOMB_3);
                AP_RemoveQueuedLocationScout(state, 0x3469420090000 | SI_ARROWS_SMALL_3);
                AP_RemoveQueuedLocationScout(state, 0x3469420090000 | SI_POTION_RED_6);
            }
            else
            {
                for (int i = SI_POTION_RED_1; i <= SI_POTION_RED_6; ++i)
                {
                    if (i == SI_BOMB_BAG_20_1 || i == SI_BOMB_BAG_40)
                    {
                        continue;
                    }
                    
                    int64_t location_id = 0x3469420090000 | i;
                    AP_RemoveQueuedLocationScout(state, location_id);
                }
                
                AP_RemoveQueuedLocationScout(state, 0x3469420090013);
                AP_RemoveQueuedLocationScout(state, 0x3469420090015);
                
                AP_RemoveQueuedLocationScout(state, 0x3469420026392);
                AP_RemoveQueuedLocationScout(state, 0x3469420090000 | GI_CHATEAU);
                AP_RemoveQueuedLocationScout(state, 0x3469420006792);
                AP_RemoveQueuedLocationScout(state, 0x3469420000091);
            }
        }
        
        if (AP_GetSlotDataInt(state, "curiostity_shop_trades") == 0)
        {
            AP_RemoveQueuedLocationScout(state, 0x346942007C402);
            AP_RemoveQueuedLocationScout(state, 0x346942007C404);
            AP_RemoveQueuedLocationScout(state, 0x346942007C405);
            AP_RemoveQueuedLocationScout(state, 0x346942007C407);
        }
        
        if (AP_GetSlotDataInt(state, "intro_checks") == 0)
        {
            AP_RemoveQueuedLocationScout(state, 0x3469420061A00);
        }
        
        AP_SendQueuedLocationScouts(state, 0);

        return true;
    }
    
    DLLEXPORT void rando_get_saved_apconnect(uint8_t* rdram, recomp_context* ctx)
    {
        PTR(char) save_dir_ptr = _arg<0, PTR(char)>(rdram, ctx);
        PTR(char) address_ptr = _arg<1, PTR(char)>(rdram, ctx);
        PTR(char) player_name_ptr = _arg<2, PTR(char)>(rdram, ctx);
        PTR(char) password_ptr = _arg<3, PTR(char)>(rdram, ctx);
        
        std::u8string save_dir;
        getU8Str(rdram, save_dir_ptr, save_dir);
        std::filesystem::path save_file_path{ save_dir };
        std::filesystem::path parent_path = save_file_path.parent_path();
        
        std::ifstream apconnect(save_file_path.parent_path().string() + "/apconnect.txt");
        
        std::string address = "archipelago.gg:38281";
        std::string player_name = "Player1";
        std::string password = "";
        
        if (apconnect.good())
        {
            address = "";
            player_name = "";
            
            glueGetLine(apconnect, address);
            glueGetLine(apconnect, player_name);
            glueGetLine(apconnect, password);
        }
        
        setStr(rdram, address_ptr, address.c_str());
        setStr(rdram, player_name_ptr, player_name.c_str());
        setStr(rdram, password_ptr, password.c_str());
    }
    
    DLLEXPORT void rando_set_saved_apconnect(uint8_t* rdram, recomp_context* ctx)
    {
        PTR(char) save_dir_ptr = _arg<0, PTR(char)>(rdram, ctx);
        PTR(char) address_ptr = _arg<1, PTR(char)>(rdram, ctx);
        PTR(char) player_name_ptr = _arg<2, PTR(char)>(rdram, ctx);
        PTR(char) password_ptr = _arg<3, PTR(char)>(rdram, ctx);
        
        std::u8string save_dir;
        getU8Str(rdram, save_dir_ptr, save_dir);
        std::filesystem::path save_file_path{ save_dir };
        std::filesystem::path parent_path = save_file_path.parent_path();
        
        std::ofstream apconnect(save_file_path.parent_path().string() + "/apconnect.txt", std::ofstream::out);
        
        std::string address = "";
        std::string player_name = "";
        std::string password = "";
        
        getStr(rdram, address_ptr, address);
        getStr(rdram, player_name_ptr, player_name);
        getStr(rdram, password_ptr, password);
        
        apconnect << address << std::endl
                  << player_name << std::endl
                  << password << std::endl;
    }
    
    DLLEXPORT void rando_init(uint8_t* rdram, recomp_context* ctx)
    {
        std::string savePath;
        std::string address;
        std::string playerName;
        std::string password;
        
        PTR(char) save_path_ptr = _arg<0, PTR(char)>(rdram, ctx);
        PTR(char) address_ptr = _arg<1, PTR(char)>(rdram, ctx);
        PTR(char) player_name_ptr = _arg<2, PTR(char)>(rdram, ctx);
        PTR(char) password_ptr = _arg<3, PTR(char)>(rdram, ctx);
        
        getStr(rdram, save_path_ptr, savePath);
        getStr(rdram, address_ptr, address);
        getStr(rdram, player_name_ptr, playerName);
        getStr(rdram, password_ptr, password);
        
        state = AP_New(savePath.c_str());
        AP_Init(state, address.c_str(), "Majora's Mask Recompiled", playerName.c_str(), password.c_str());

        bool success = rando_init_common();
        if (success) {
            AP_RoomInfo roomInfo{};
            AP_GetRoomInfo(state, &roomInfo);
            room_seed_name = std::u8string{ reinterpret_cast<const char8_t*>(roomInfo.seed_name.data()), roomInfo.seed_name.size() };
        }

        _return<u32>(ctx, success);
    }
    
    DLLEXPORT void rando_init_solo(uint8_t* rdram, recomp_context* ctx)
    {
        std::string savePath;

        PTR(char) save_path_ptr = _arg<0, PTR(char)>(rdram, ctx);
        u32 selected_seed = _arg<1, u32>(rdram, ctx);

        getStr(rdram, save_path_ptr, savePath);
        
        if (selected_seed >= solo_state.seeds.size())
        {
            _return<u32>(ctx, false);
            return;
        }
        
        state = AP_New(savePath.c_str());
        const std::u8string& seed = solo_state.seeds[selected_seed].seed_name;
        std::filesystem::path gen_file = solo_state.seed_folder / (std::u8string{ gen_file_prefix } + seed + std::u8string{ gen_file_suffix });
        AP_InitSolo(state, reinterpret_cast<const char*>(gen_file.u8string().c_str()), reinterpret_cast<const char*>(seed.c_str()));
        
        bool success = rando_init_common();
        if (success)
        {
            room_seed_name = seed;
        }
        
        _return<u32>(ctx, success);
    }

    DLLEXPORT void rando_scan_solo_seeds(uint8_t* rdram, recomp_context* ctx)
    {
        std::u8string save_file_path_str;
        PTR(char) save_file_path_ptr = _arg<0, PTR(char)>(rdram, ctx);
        
        getU8Str(rdram, save_file_path_ptr, save_file_path_str);
        
        std::filesystem::path save_file_path{ save_file_path_str };
        
        solo_state.seed_folder = save_file_path.parent_path();
        solo_state.seeds.clear();
        
        for (const auto& file : std::filesystem::directory_iterator{ solo_state.seed_folder })
        {
            std::error_code ec;
            if (file.is_regular_file(ec))
            {
                std::filesystem::path filename = file.path().filename();
                std::u8string filename_str = filename.u8string();
                if (filename_str.starts_with(gen_file_prefix) && filename_str.ends_with(gen_file_suffix))
                {
                    // TODO use platform-specific APIs to get the actual file creation time instead of using the last write time.
                    std::filesystem::file_time_type timestamp = std::filesystem::last_write_time(file);
                    
                    solo_state.seeds.emplace_back(SoloSeed {
                        .seed_name = filename_str.substr(gen_file_prefix.size(), filename_str.size() - gen_file_prefix.size() - gen_file_suffix.size()),
                        .timestamp = timestamp,
                        .date_string = format_file_time(timestamp)
                    });
                }
            }
        }
        
        // Sort the seeds by timestamp descending.
        std::sort(solo_state.seeds.begin(), solo_state.seeds.end(),
            [](const SoloSeed& lhs, const SoloSeed& rhs)
            {
                return lhs.timestamp > rhs.timestamp;
            }
        );
    }
    
    DLLEXPORT void rando_solo_count(uint8_t* rdram, recomp_context* ctx)
    {
        _return(ctx, static_cast<u32>(solo_state.seeds.size()));
    }

    DLLEXPORT void rando_solo_get_seed_name(uint8_t* rdram, recomp_context* ctx)
    {
        u32 seed_index = _arg<0, u32>(rdram, ctx);
        PTR(char) seed_name_out = _arg<1, PTR(char)>(rdram, ctx);
        u32 seed_name_out_len = _arg<2, u32>(rdram, ctx);
        
        if (seed_index >= solo_state.seeds.size())
        {
            _return<u32>(ctx, 0);
            return;
        }
        
        const std::u8string& solo_seed_name = solo_state.seeds[seed_index].seed_name;
        u32 seed_name_size = static_cast<u32>(solo_seed_name.size() + 1);
        
        if (seed_name_out_len == 0)
        {
            // Write nothing if the output length is 0.
        }
        
        else if (solo_seed_name.size() + 1 >= seed_name_out_len)
        {
            setU8Str(rdram, seed_name_out, solo_seed_name.substr(0, seed_name_out_len - 1).c_str());
        }
        
        else
        {
            setU8Str(rdram, seed_name_out, solo_seed_name.c_str());
        }
        
        _return<u32>(ctx, seed_name_size);
    }

    DLLEXPORT void rando_solo_get_generation_date(uint8_t* rdram, recomp_context* ctx)
    {
        u32 seed_index = _arg<0, u32>(rdram, ctx);
        PTR(char) seed_date_out = _arg<1, PTR(char)>(rdram, ctx);
        u32 seed_date_out_len = _arg<2, u32>(rdram, ctx);
        
        if (seed_index >= solo_state.seeds.size())
        {
            _return<u32>(ctx, 0);
            return;
        }
        
        const std::string& seed_date = solo_state.seeds[seed_index].date_string;
        u32 seed_date_size = static_cast<u32>(seed_date.size() + 1);
        
        if (seed_date_out_len == 0)
        {
            // Write nothing if the output length is 0.
        }
        
        else if (seed_date.size() + 1 >= seed_date_out_len)
        {
            setStr(rdram, seed_date_out, seed_date.substr(0, seed_date_out_len - 1).c_str());
        }
        
        else
        {
            setStr(rdram, seed_date_out, seed_date.c_str());
        }
        
        _return<u32>(ctx, seed_date_size);
    }
    
    DLLEXPORT void rando_get_seed_name(uint8_t* rdram, recomp_context* ctx)
    {
        PTR(char) seed_name_out = _arg<0, PTR(char)>(rdram, ctx);
        u32 seed_name_out_len = _arg<1, u32>(rdram, ctx);
        
        u32 seed_name_size = static_cast<u32>(room_seed_name.size() + 1);
        
        if (seed_name_out_len == 0)
        {
            // Write nothing if the output length is 0.
        }
        
        else if (room_seed_name.size() + 1 >= seed_name_out_len)
        {
            setU8Str(rdram, seed_name_out, room_seed_name.substr(0, seed_name_out_len - 1).c_str());
        }
        
        else
        {
            setU8Str(rdram, seed_name_out, room_seed_name.c_str());
        }
        
        _return<u32>(ctx, seed_name_size);
    }
    
    DLLEXPORT void rando_solo_generate(uint8_t* rdram, recomp_context* ctx)
    {
        _return<u32>(ctx, sologen::generate(solo_state.seed_folder / sologen::yaml_folder, solo_state.seed_folder));
    }
    
    DLLEXPORT void rando_skulltulas_enabled(uint8_t* rdram, recomp_context* ctx)
    {
        _return(ctx, AP_GetSlotDataInt(state, "skullsanity") != 2);
    }
    
    DLLEXPORT void rando_shopsanity_enabled(uint8_t* rdram, recomp_context* ctx)
    {
        _return(ctx, AP_GetSlotDataInt(state, "shopsanity") != 0);
    }
    
    DLLEXPORT void rando_advanced_shops_enabled(uint8_t* rdram, recomp_context* ctx)
    {
        _return(ctx, AP_GetSlotDataInt(state, "shopsanity") == 2);
    }

    DLLEXPORT void rando_scrubs_enabled(uint8_t* rdram, recomp_context* ctx)
    {
        _return(ctx, AP_GetSlotDataInt(state, "scrubsanity") == 1);
    }

    DLLEXPORT void rando_cows_enabled(uint8_t* rdram, recomp_context* ctx)
    {
        _return(ctx, AP_GetSlotDataInt(state, "cowsanity") == 1);
    }
    
    DLLEXPORT void rando_damage_multiplier(uint8_t* rdram, recomp_context* ctx)
    {
        switch (AP_GetSlotDataInt(state, "damage_multiplier"))
        {
            case 0:
                _return(ctx, (u32) 0);
                return;
            case 1:
                _return(ctx, (u32) 1);
                return;
            case 2:
                _return(ctx, (u32) 2);
                return;
            case 3:
                _return(ctx, (u32) 4);
                return;
            case 4:
                _return(ctx, (u32) 0xF);
                return;
        }
        return;
    }
    
    DLLEXPORT void rando_death_behavior(uint8_t* rdram, recomp_context* ctx)
    {
        _return(ctx, (u32) AP_GetSlotDataInt(state, "death_behavior"));
    }

    DLLEXPORT void rando_get_death_link_pending(uint8_t* rdram, recomp_context* ctx)
    {
        _return(ctx, AP_DeathLinkPending(state));
    }
    
    DLLEXPORT void rando_reset_death_link_pending(uint8_t* rdram, recomp_context* ctx)
    {
        AP_DeathLinkClear(state);
    }
    
    DLLEXPORT void rando_get_death_link_enabled(uint8_t* rdram, recomp_context* ctx)
    {
        _return(ctx, AP_GetSlotDataInt(state, "death_link") == 1);
    }
    
    DLLEXPORT void rando_send_death_link(uint8_t* rdram, recomp_context* ctx)
    {
        AP_DeathLinkSend(state);
    }
    
    DLLEXPORT void rando_get_camc_enabled(uint8_t* rdram, recomp_context* ctx)
    {
        _return(ctx, AP_GetSlotDataInt(state, "camc") == 1);
    }
       
    DLLEXPORT void rando_is_magic_trap(uint8_t* rdram, recomp_context* ctx)
    {
        _return(ctx, AP_GetSlotDataInt(state, "magic_is_a_trap") == 1);
    }

    DLLEXPORT void rando_get_start_with_consumables_enabled(uint8_t* rdram, recomp_context* ctx)
    {
        _return(ctx, AP_GetSlotDataInt(state, "start_with_consumables") == 1);
    }
    
    DLLEXPORT void rando_get_permanent_chateau_romani_enabled(uint8_t* rdram, recomp_context* ctx)
    {
        _return(ctx, AP_GetSlotDataInt(state, "permanent_chateau_romani") == 1);
    }
    
    DLLEXPORT void rando_get_start_with_inverted_time_enabled(uint8_t* rdram, recomp_context* ctx)
    {
        _return(ctx, AP_GetSlotDataInt(state, "start_with_inverted_time") == 1);
    }
    
    DLLEXPORT void rando_get_receive_filled_wallets_enabled(uint8_t* rdram, recomp_context* ctx)
    {
        _return(ctx, AP_GetSlotDataInt(state, "receive_filled_wallets") == 1);
    }
    
    DLLEXPORT void rando_get_remains_allow_boss_warps_enabled(uint8_t* rdram, recomp_context* ctx)
    {
        _return(ctx, (int) AP_GetSlotDataInt(state, "remains_allow_boss_warps"));
    }
    
    DLLEXPORT void rando_get_starting_heart_locations(uint8_t* rdram, recomp_context* ctx)
    {
        _return(ctx, (int) AP_GetSlotDataInt(state, "starting_heart_locations"));
    }
    
    DLLEXPORT void rando_get_moon_remains_required(uint8_t* rdram, recomp_context* ctx)
    {
        _return(ctx, (int) AP_GetSlotDataInt(state, "moon_remains_required"));
    }
    
    DLLEXPORT void rando_get_majora_remains_required(uint8_t* rdram, recomp_context* ctx)
    {
        _return(ctx, (int) AP_GetSlotDataInt(state, "majora_remains_required"));
    }
    
    DLLEXPORT void rando_get_random_seed(uint8_t* rdram, recomp_context* ctx)
    {
        _return(ctx, (u32) AP_GetSlotDataInt(state, "random_seed"));
    }
    
    DLLEXPORT void rando_get_curiostity_shop_trades(uint8_t* rdram, recomp_context* ctx)
    {
        _return(ctx, (int) AP_GetSlotDataInt(state, "curiostity_shop_trades"));
    }
    
    DLLEXPORT void rando_get_tunic_color(uint8_t* rdram, recomp_context* ctx)
    {
        _return(ctx, (int) AP_GetSlotDataInt(state, "link_tunic_color"));
    }
    
    DLLEXPORT void rando_get_shop_price(uint8_t* rdram, recomp_context* ctx)
    {
        u32 arg = _arg<0, u32>(rdram, ctx);
        _return(ctx, (s16) prices[arg]);
    }
    
    DLLEXPORT void rando_get_location_type(uint8_t* rdram, recomp_context* ctx)
    {
        u32 arg = _arg<0, u32>(rdram, ctx);
        int64_t location = 0x3469420000000 | fixLocation(arg);
        _return(ctx, (int) AP_GetLocationItemType(state, location));
    }
    
    DLLEXPORT void rando_get_item_id(uint8_t* rdram, recomp_context* ctx)
    {
        u32 arg = _arg<0, u32>(rdram, ctx);
        
        if (arg == 0)
        {
            _return(ctx, 0);
            return;
        }
        
        int64_t location = 0x3469420000000 | fixLocation(arg);
        
        if (AP_GetLocationHasLocalItem(state, location))
        {
            int64_t item = AP_GetItemAtLocation(state, location) & 0xFFFFFF;
            
            if ((item & 0xFF0000) == 0x000000)
            {
                u8 gi = item & 0xFF;
                
                if (gi == GI_SWORD_KOKIRI)
                {
                    _return(ctx, (u32) MIN(GI_SWORD_KOKIRI + hasItem(0x3469420000000 | GI_SWORD_KOKIRI), GI_SWORD_GILDED));
                    return;
                }
                
                else if (gi == GI_QUIVER_30)
                {
                    _return(ctx, (u32) MIN(GI_QUIVER_30 + hasItem(0x3469420000000 | GI_QUIVER_30), GI_QUIVER_50));
                    return;
                }
                
                else if (gi == GI_BOMB_BAG_20)
                {
                    _return(ctx, (u32) MIN(GI_BOMB_BAG_20 + hasItem(0x3469420000000 | GI_BOMB_BAG_20), GI_BOMB_BAG_40));
                    return;
                }
                
                else if (gi == GI_WALLET_ADULT)
                {
                    _return(ctx, (u32) MIN(GI_WALLET_ADULT + hasItem(0x3469420000000 | GI_WALLET_ADULT), GI_WALLET_GIANT));
                    return;
                }
                
                _return(ctx, (u32) gi);
                return;
            }
            switch (item & 0xFF0000)
            {
                case 0x010000:
                    switch (item & 0xFF)
                    {
                        case 0x7F:
                            _return(ctx, (u32) GI_B2);
                            return;
                        case 0x00:
                            _return(ctx, (u32) GI_46);
                            return;
                        case 0x01:
                            _return(ctx, (u32) GI_47);
                            return;
                        case 0x02:
                            _return(ctx, (u32) GI_48);
                            return;
                        case 0x03:
                            _return(ctx, (u32) GI_49);
                            return;
                    }
                    return;
                case 0x020000:
                    switch (item & 0xFF)
                    {
                        case 0x00:
                            _return(ctx, (u32) GI_MAGIC_JAR_SMALL);
                            return;
                        case 0x01:
                            _return(ctx, (u32) GI_71);
                            return;
                        case 0x03:
                            _return(ctx, (u32) GI_73);
                            return;
                    }
                    return;
                case 0x040000:
                    switch (item & 0xFF)
                    {
                        case ITEM_SONG_TIME:
                            _return(ctx, (u32) GI_A6);
                            return;
                        case ITEM_SONG_HEALING:
                            _return(ctx, (u32) GI_AF);
                            return;
                        case ITEM_SONG_EPONA:
                            _return(ctx, (u32) GI_A5);
                            return;
                        case ITEM_SONG_SOARING:
                            _return(ctx, (u32) GI_A3);
                            return;
                        case ITEM_SONG_STORMS:
                            _return(ctx, (u32) GI_A2);
                            return;
                        case ITEM_SONG_SONATA:
                            _return(ctx, (u32) GI_AE);
                            return;
                        case ITEM_SONG_LULLABY:
                            _return(ctx, (u32) GI_AD);
                            return;
                        case ITEM_SONG_NOVA:
                            _return(ctx, (u32) GI_AC);
                            return;
                        case ITEM_SONG_ELEGY:
                            _return(ctx, (u32) GI_A8);
                            return;
                        case ITEM_SONG_OATH:
                            _return(ctx, (u32) GI_A7);
                            return;
                    }
                    return;
                case 0x090000:
                    switch (item & 0xFF)
                    {
                        case ITEM_KEY_BOSS:
                            _return(ctx, (u32) (GI_MAX + (((item >> 8) & 0xF) * 4) + 1));
                            return;
                        case ITEM_KEY_SMALL:
                            _return(ctx, (u32) (GI_MAX + (((item >> 8) & 0xF) * 4) + 2));
                            return;
                        case ITEM_DUNGEON_MAP:
                            _return(ctx, (u32) (GI_MAX + (((item >> 8) & 0xF) * 4) + 3));
                            return;
                        case ITEM_COMPASS:
                            _return(ctx, (u32) (GI_MAX + (((item >> 8) & 0xF) * 4) + 4));
                            return;
                    }
                    return;
            }
        }
        
        switch (AP_GetLocationItemType(state, location))
        {
            case ITEM_TYPE_FILLER:
                _return(ctx, (u32) GI_AP_FILLER);
                return;
            case ITEM_TYPE_USEFUL:
                _return(ctx, (u32) GI_AP_USEFUL);
                return;
            default:
                _return(ctx, (u32) GI_AP_PROG);
                return;
        }
    }
    
    DLLEXPORT void rando_get_slotdata_u32(uint8_t* rdram, recomp_context* ctx)
    {
        PTR(char) ptr = _arg<0, PTR(char)>(rdram, ctx);

        std::string key = "";
        getStr(rdram, ptr, key);
        u32 value = (u32) (AP_GetSlotDataInt(state, key.c_str()) & 0xFFFFFFFF);

        _return(ctx, value);
    }
    
    DLLEXPORT void rando_get_slotdata_string(uint8_t* rdram, recomp_context* ctx)
    {
        PTR(char) ptr = _arg<0, PTR(char)>(rdram, ctx);
        PTR(char) ret_ptr = _arg<1, PTR(char)>(rdram, ctx);

        std::string key = "";
        getStr(rdram, ptr, key);
        const char* value = AP_GetSlotDataString(state, key.c_str());

        setStr(rdram, ret_ptr, value);
    }
    
    DLLEXPORT void rando_get_slotdata_raw_o32(uint8_t* rdram, recomp_context* ctx)
    {
        PTR(char) key_ptr = _arg<0, PTR(char)>(rdram, ctx);
        PTR(u32) out_ptr = _arg<1, PTR(u32)>(rdram, ctx);
        
        std::string key;
        getStr(rdram, key_ptr, key);
        
        uintptr_t jsonValue = AP_GetSlotDataRaw(state, key.c_str());
        
        MEM_W(out_ptr, 0) = UPPER(jsonValue);
        MEM_W(out_ptr, 4) = LOWER(jsonValue);
    }
    
    DLLEXPORT void rando_access_slotdata_raw_array_o32(uint8_t* rdram, recomp_context* ctx)
    {
        PTR(u32) in_ptr = _arg<0, u32>(rdram, ctx);
        u32 index = _arg<1, u32>(rdram, ctx);
        PTR(u32) out_ptr = _arg<2, PTR(u32)>(rdram, ctx);
        
        u32 upper = MEM_W(in_ptr, 0);
        u32 lower = MEM_W(in_ptr, 4);
        
        uintptr_t jsonValue = CRAFT_64(upper, lower);
        jsonValue = AP_AccessSlotDataRawArray(state, jsonValue, index);
        
        MEM_W(out_ptr, 0) = UPPER(jsonValue);
        MEM_W(out_ptr, 4) = LOWER(jsonValue);
    }
    
    DLLEXPORT void rando_access_slotdata_raw_dict_o32(uint8_t* rdram, recomp_context* ctx)
    {
        PTR(u32) in_ptr = _arg<0, u32>(rdram, ctx);
        PTR(char) key_ptr = _arg<1, PTR(char)>(rdram, ctx);
        PTR(u32) out_ptr = _arg<2, PTR(u32)>(rdram, ctx);
        
        u32 upper = MEM_W(in_ptr, 0);
        u32 lower = MEM_W(in_ptr, 4);
        
        std::string key;
        getStr(rdram, key_ptr, key);
        
        uintptr_t jsonValue = CRAFT_64(upper, lower);
        jsonValue = AP_AccessSlotDataRawDict(state, jsonValue, key.c_str());
        
        MEM_W(out_ptr, 0) = UPPER(jsonValue);
        MEM_W(out_ptr, 4) = LOWER(jsonValue);
    }
    
    DLLEXPORT void rando_access_slotdata_raw_u32_o32(uint8_t* rdram, recomp_context* ctx)
    {
        PTR(u32) in_ptr = _arg<0, u32>(rdram, ctx);
        
        u32 upper = MEM_W(in_ptr, 0);
        u32 lower = MEM_W(in_ptr, 4);
        
        uintptr_t jsonValue = CRAFT_64(upper, lower);
        
        _return(ctx, (u32) (AP_AccessSlotDataRawInt(state, jsonValue) & 0xFFFFFFFF));
    }
    
    DLLEXPORT void rando_access_slotdata_raw_string_o32(uint8_t* rdram, recomp_context* ctx)
    {
        PTR(u32) in_ptr = _arg<0, u32>(rdram, ctx);
        PTR(char) str_ptr = _arg<1, PTR(char)>(rdram, ctx);
        
        u32 upper = MEM_W(in_ptr, 0);
        u32 lower = MEM_W(in_ptr, 4);
        
        uintptr_t jsonValue = CRAFT_64(upper, lower);
        
        setStr(rdram, str_ptr, AP_AccessSlotDataRawString(state, jsonValue));
    }
    
    DLLEXPORT void rando_get_datastorage_u32_sync(uint8_t* rdram, recomp_context* ctx)
    {
        PTR(char) ptr = _arg<0, PTR(char)>(rdram, ctx);

        std::string key = "";
        getStr(rdram, ptr, key);
        key += "_P" + std::to_string(AP_GetPlayerID(state));
        char* value_char_ptr = AP_GetDataStorageSync(state, key.c_str());

        u32 value = 0;

        if (strncmp(value_char_ptr, "null", 4) != 0)
        {
            value = std::stoi(value_char_ptr);
        }

        _return(ctx, value);
    }
    
    DLLEXPORT void rando_get_global_datastorage_u32_sync(uint8_t* rdram, recomp_context* ctx)
    {
        PTR(char) ptr = _arg<0, PTR(char)>(rdram, ctx);

        std::string key = "";
        getStr(rdram, ptr, key);
        char* value_char_ptr = AP_GetDataStorageSync(state, key.c_str());

        u32 value = 0;

        if (strncmp(value_char_ptr, "null", 4) != 0)
        {
            value = std::stoi(value_char_ptr);
        }

        _return(ctx, value);
    }
    
    DLLEXPORT void rando_get_datastorage_string_sync(uint8_t* rdram, recomp_context* ctx)
    {
        PTR(char) ptr = _arg<0, PTR(char)>(rdram, ctx);
        PTR(char) ret_ptr = _arg<1, PTR(char)>(rdram, ctx);

        std::string key = "";
        getStr(rdram, ptr, key);
        key += "_P" + std::to_string(AP_GetPlayerID(state));
        char* value = AP_GetDataStorageSync(state, key.c_str());

        setStr(rdram, ret_ptr, value);
    }
    
    DLLEXPORT void rando_get_global_datastorage_string_sync(uint8_t* rdram, recomp_context* ctx)
    {
        PTR(char) ptr = _arg<0, PTR(char)>(rdram, ctx);
        PTR(char) ret_ptr = _arg<1, PTR(char)>(rdram, ctx);

        std::string key = "";
        getStr(rdram, ptr, key);
        char* value = AP_GetDataStorageSync(state, key.c_str());

        setStr(rdram, ret_ptr, value);
    }
    
    DLLEXPORT void rando_set_datastorage_u32_sync(uint8_t* rdram, recomp_context* ctx)
    {
        PTR(char) ptr = _arg<0, PTR(char)>(rdram, ctx);
        u32 value = _arg<1, u32>(rdram, ctx);
        std::string key = "";
        getStr(rdram, ptr, key);
        key += "_P" + std::to_string(AP_GetPlayerID(state));

        try
        {
            AP_SetDataStorageSync(state, key.c_str(), (char*) std::to_string(value).c_str());
        }

        catch (std::exception e)
        {
            fprintf(stderr, "error setting datastorage u32\n");
            fprintf(stderr, e.what());
        }
    }
    
    DLLEXPORT void rando_set_global_datastorage_u32_sync(uint8_t* rdram, recomp_context* ctx)
    {
        PTR(char) ptr = _arg<0, PTR(char)>(rdram, ctx);
        u32 value = _arg<1, u32>(rdram, ctx);
        std::string key = "";
        getStr(rdram, ptr, key);

        try
        {
            AP_SetDataStorageSync(state, key.c_str(), (char*) std::to_string(value).c_str());
        }

        catch (std::exception e)
        {
            fprintf(stderr, "error setting datastorage u32\n");
            fprintf(stderr, e.what());
        }
    }
    
    DLLEXPORT void rando_set_datastorage_u32_async(uint8_t* rdram, recomp_context* ctx)
    {
        PTR(char) ptr = _arg<0, PTR(char)>(rdram, ctx);
        u32 value = _arg<1, u32>(rdram, ctx);
        std::string key = "";
        getStr(rdram, ptr, key);
        key += "_P" + std::to_string(AP_GetPlayerID(state));

        try
        {
            AP_SetDataStorageAsync(state, key.c_str(), (char*) std::to_string(value).c_str());
        }

        catch (std::exception e)
        {
            fprintf(stderr, "error setting datastorage u32\n");
            fprintf(stderr, e.what());
        }
    }
    
    DLLEXPORT void rando_set_global_datastorage_u32_async(uint8_t* rdram, recomp_context* ctx)
    {
        PTR(char) ptr = _arg<0, PTR(char)>(rdram, ctx);
        u32 value = _arg<1, u32>(rdram, ctx);
        std::string key = "";
        getStr(rdram, ptr, key);

        try
        {
            AP_SetDataStorageAsync(state, key.c_str(), (char*) std::to_string(value).c_str());
        }

        catch (std::exception e)
        {
            fprintf(stderr, "error setting datastorage u32\n");
            fprintf(stderr, e.what());
        }
    }
    
    DLLEXPORT void rando_set_datastorage_string_sync(uint8_t* rdram, recomp_context* ctx)
    {
        PTR(char) ptr = _arg<0, PTR(char)>(rdram, ctx);
        PTR(char) value_ptr = _arg<1, PTR(char)>(rdram, ctx);
        
        std::string key = "";
        getStr(rdram, ptr, key);
        
        std::string value = "";
        getStr(rdram, value_ptr, value);
        
        key += "_P" + std::to_string(AP_GetPlayerID(state));

        try
        {
            AP_SetDataStorageSync(state, key.c_str(), (char*) value.c_str());
        }

        catch (std::exception e)
        {
            fprintf(stderr, "error setting datastorage u32\n");
            fprintf(stderr, e.what());
        }
    }
    
    DLLEXPORT void rando_set_global_datastorage_string_sync(uint8_t* rdram, recomp_context* ctx)
    {
        PTR(char) ptr = _arg<0, PTR(char)>(rdram, ctx);
        PTR(char) value_ptr = _arg<1, PTR(char)>(rdram, ctx);
        
        std::string key = "";
        getStr(rdram, ptr, key);
        
        std::string value = "";
        getStr(rdram, value_ptr, value);

        try
        {
            AP_SetDataStorageSync(state, key.c_str(), (char*) value.c_str());
        }

        catch (std::exception e)
        {
            fprintf(stderr, "error setting datastorage u32\n");
            fprintf(stderr, e.what());
        }
    }
    
    DLLEXPORT void rando_set_datastorage_string_async(uint8_t* rdram, recomp_context* ctx)
    {
        PTR(char) ptr = _arg<0, PTR(char)>(rdram, ctx);
        PTR(char) value_ptr = _arg<1, PTR(char)>(rdram, ctx);
        
        std::string key = "";
        getStr(rdram, ptr, key);
        
        std::string value = "";
        getStr(rdram, value_ptr, value);
        
        key += "_P" + std::to_string(AP_GetPlayerID(state));

        try
        {
            AP_SetDataStorageAsync(state, key.c_str(), (char*) value.c_str());
        }

        catch (std::exception e)
        {
            fprintf(stderr, "error setting datastorage u32\n");
            fprintf(stderr, e.what());
        }
    }
    
    DLLEXPORT void rando_set_global_datastorage_string_async(uint8_t* rdram, recomp_context* ctx)
    {
        PTR(char) ptr = _arg<0, PTR(char)>(rdram, ctx);
        PTR(char) value_ptr = _arg<1, PTR(char)>(rdram, ctx);
        
        std::string key = "";
        getStr(rdram, ptr, key);
        
        std::string value = "";
        getStr(rdram, value_ptr, value);

        try
        {
            AP_SetDataStorageAsync(state, key.c_str(), (char*) value.c_str());
        }

        catch (std::exception e)
        {
            fprintf(stderr, "error setting datastorage u32\n");
            fprintf(stderr, e.what());
        }
    }
    
    DLLEXPORT void rando_get_own_slot_id(uint8_t* rdram, recomp_context* ctx)
    {
        _return(ctx, ((u32) AP_GetPlayerID(state)));
    }
    
    DLLEXPORT void rando_get_own_slot_name(uint8_t* rdram, recomp_context* ctx)
    {
        PTR(char) str_ptr = _arg<0, PTR(char)>(rdram, ctx);
        setStr(rdram, str_ptr, AP_GetPlayerName(state));
    }
    
    DLLEXPORT void rando_get_location_item_player(uint8_t* rdram, recomp_context* ctx)
    {
        u32 location_id_arg = _arg<0, u32>(rdram, ctx);
        PTR(char) str_ptr = _arg<1, PTR(char)>(rdram, ctx);
        
        int64_t location_id = ((int64_t) (((int64_t) 0x3469420000000) | ((int64_t) fixLocation(location_id_arg))));
        
        setStr(rdram, str_ptr, AP_GetLocationItemPlayer(state, location_id));
    }
    
    DLLEXPORT void rando_get_location_item_name(uint8_t* rdram, recomp_context* ctx)
    {
        u32 location_id_arg = _arg<0, u32>(rdram, ctx);
        PTR(char) str_ptr = _arg<1, PTR(char)>(rdram, ctx);
        
        int64_t location_id = ((int64_t) (((int64_t) 0x3469420000000) | ((int64_t) fixLocation(location_id_arg))));
        
        setStr(rdram, str_ptr, AP_GetLocationItemName(state, location_id));
    }
    
    DLLEXPORT void rando_get_items_size(uint8_t* rdram, recomp_context* ctx)
    {
        _return(ctx, ((u32) AP_GetReceivedItemsSize(state)));
    }
    
    DLLEXPORT void rando_get_item(uint8_t* rdram, recomp_context* ctx)
    {
        u32 items_i = _arg<0, u32>(rdram, ctx);
        _return(ctx, ((u32) AP_GetReceivedItem(state, items_i)));
    }
    
    DLLEXPORT void rando_get_item_location(uint8_t* rdram, recomp_context* ctx)
    {
        u32 items_i = _arg<0, u32>(rdram, ctx);
        _return(ctx, ((s32) AP_GetReceivedItemLocation(state, items_i)));
    }
    
    DLLEXPORT void rando_get_sending_player(uint8_t* rdram, recomp_context* ctx)
    {
        u32 items_i = _arg<0, u32>(rdram, ctx);
        _return(ctx, ((u32) AP_GetSendingPlayer(state, items_i) & 0xFFFFFFFF));
    }
    
    DLLEXPORT void rando_get_item_name_from_id(uint8_t* rdram, recomp_context* ctx)
    {
        u32 arg = _arg<0, u32>(rdram, ctx);
        PTR(char) str_ptr = _arg<1, PTR(char)>(rdram, ctx);
        
        int64_t item_id = ((int64_t) (((int64_t) 0x3469420000000) | ((int64_t) arg)));
        
        setStr(rdram, str_ptr, AP_GetItemNameFromID(state, item_id));
    }
    
    DLLEXPORT void rando_get_sending_player_name(uint8_t* rdram, recomp_context* ctx)
    {
        u32 items_i = _arg<0, u32>(rdram, ctx);
        PTR(char) str_ptr = _arg<1, PTR(char)>(rdram, ctx);
        
        int64_t sending_player = AP_GetSendingPlayer(state, items_i);
        
        setStr(rdram, str_ptr, AP_GetPlayerFromSlot(state, sending_player));
    }
    
    DLLEXPORT void rando_has_item(uint8_t* rdram, recomp_context* ctx)
    {
        u32 arg = _arg<0, u32>(rdram, ctx);
        int64_t item_id = ((int64_t) (((int64_t) 0x3469420000000) | ((int64_t) arg)));
        _return(ctx, hasItem(item_id));
    }
    
    DLLEXPORT void rando_has_item_async(uint8_t* rdram, recomp_context* ctx)
    {
        u32 arg = _arg<0, u32>(rdram, ctx);
        int64_t item_id = ((int64_t) (((int64_t) 0x3469420000000) | ((int64_t) arg)));
        _return(ctx, hasItem(item_id));
    }
    
    DLLEXPORT void rando_broadcast_location_hint(uint8_t* rdram, recomp_context* ctx)
    {
        u32 arg = _arg<0, u32>(rdram, ctx);
        int64_t location_id = ((int64_t) (((int64_t) 0x3469420000000) | ((int64_t) fixLocation(arg))));
        AP_QueueLocationScout(state, location_id);
        AP_SendQueuedLocationScouts(state, 2);
    }
    
    DLLEXPORT void rando_send_location(uint8_t* rdram, recomp_context* ctx)
    {
        u32 arg = _arg<0, u32>(rdram, ctx);
        int64_t location_id = ((int64_t) (((int64_t) 0x3469420000000) | ((int64_t) fixLocation(arg))));
        if (AP_LocationExists(state, location_id))
        {
            last_location_sent = location_id;
            if (!AP_GetLocationIsChecked(state, location_id))
            {
                AP_SendItem(state, location_id);
            }
        }
    }
    
    DLLEXPORT void rando_location_is_checked(uint8_t* rdram, recomp_context* ctx)
    {
        u32 arg = _arg<0, u32>(rdram, ctx);
        int64_t location_id = ((int64_t) (((int64_t) 0x3469420000000) | ((int64_t) fixLocation(arg))));
        _return(ctx, AP_GetLocationIsChecked(state, location_id));
    }
    
    DLLEXPORT void rando_location_is_checked_async(uint8_t* rdram, recomp_context* ctx)
    {
        u32 arg = _arg<0, u32>(rdram, ctx);
        int64_t location_id = ((int64_t) (((int64_t) 0x3469420000000) | ((int64_t) fixLocation(arg))));
        _return(ctx, AP_GetLocationIsChecked(state, location_id));
    }
    
    DLLEXPORT void rando_get_last_location_sent(uint8_t* rdram, recomp_context* ctx)
    {
        _return(ctx, (u32) (last_location_sent & 0xFFFFFF));
    }
    
    DLLEXPORT void rando_complete_goal(uint8_t* rdram, recomp_context* ctx)
    {
        AP_StoryComplete(state);
    }
}
