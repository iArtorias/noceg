/*
 * This software is licensed under the NoCEG Non-Commercial Copyleft License.
 *
 * Copyright (C) 2025 iArtorias <iartorias.re@gmail.com>
 *
 * You may use, copy, modify, and distribute this software non-commercially only.
 * If you distribute binaries or run it as a service, you must also provide
 * the full source code under the same license.
 *
 * This software is provided "as is", without warranty of any kind.
 *
 * Full license text available in LICENSE.md
 */

#include <windows.h>
#include <iostream>
#include <cstdint>
#include <fstream>
#include <vector>
#include <string>
#include <filesystem>
#include <map>
#include <iomanip>
#include <charconv>
#include <format>

// JSON for Modern C++ (https://github.com/nlohmann/json)
#include "json.hpp"

namespace fs = std::filesystem;

using json = nlohmann::json;

// Structure to hold CEG patch information.
struct PatchInfo
{
    // The memory address where the patch should be applied.
    std::string m_Prologue;

    // Type of patch to apply.
    int m_Type { 0 };

    // Type of the CEG function.
    std::string m_Value;
};

class Patcher
{
private:

    // Raw binary data of the loaded PE file.
    std::vector<std::uint8_t> m_FileData {};

    // Base address where the PE image is loaded in memory.
    std::uintptr_t m_ImageBase { 0 };
    
    /**
    * @brief Converts a hexadecimal string to a numeric value
    *
    * @param hex The hexadecimal string to convert.
    * @return The numeric value, or '0' if conversion fails.
    */
    [[nodiscard]] static std::uintptr_t StringToNumber( 
        std::string_view hex ) noexcept
    {
        if (hex.starts_with( "0x" ))
            hex.remove_prefix( 2 );

        std::uintptr_t value {};
        const auto [ptr, ec] = std::from_chars( hex.data(), hex.data() + hex.size(), value, 16 );
        return (ec == std::errc()) ? value : 0;
    }
    
    
    /**
    * @brief Validates if the loaded file is a valid PE file.
    *
    * @return true if the file is a valid PE, false otherwise.
    */
    [[nodiscard]] bool IsValidPe() const noexcept
    {
        if (m_FileData.size() < sizeof( IMAGE_DOS_HEADER )) 
            return false;

        const auto * dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(m_FileData.data());
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) 
            return false;

        if (m_FileData.size() < static_cast<std::size_t>(dos->e_lfanew) + sizeof( IMAGE_NT_HEADERS )) 
            return false;

        const auto * nt_headers = reinterpret_cast<const IMAGE_NT_HEADERS *>(m_FileData.data() + dos->e_lfanew);
        return nt_headers->Signature == IMAGE_NT_SIGNATURE;
    }
    
    
    /**
    * @brief Extracts the image base address from the PE headers.
    *
    * @return true if image base was successfully extracted, false otherwise
    */
    [[nodiscard]] bool GetImageBase() noexcept
    {
        const auto * dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(m_FileData.data());
        const auto * nt_headers = reinterpret_cast<const IMAGE_NT_HEADERS *>(m_FileData.data() + dos->e_lfanew);

        m_ImageBase = nt_headers->OptionalHeader.ImageBase;
        return true;
    }
    
    
    /**
    * @brief Converts RVA to a file offset.
    *
    * @param rva The relative virtual address to convert.
    * @return The corresponding file offset, or 0 if the RVA is invalid.
    */
    [[nodiscard]] std::uint32_t RvaToOffset( 
        std::uint32_t rva
    ) const noexcept
    {
        const auto * dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(m_FileData.data());
        const auto * nt_headers = reinterpret_cast<const IMAGE_NT_HEADERS *>(m_FileData.data() + dos->e_lfanew);

        const auto * section = IMAGE_FIRST_SECTION( nt_headers );
        for (unsigned i = 0; i < nt_headers->FileHeader.NumberOfSections; ++i)
        {
            if (rva >= section[i].VirtualAddress &&
                rva < section[i].VirtualAddress + section[i].Misc.VirtualSize)
                return rva - section[i].VirtualAddress + section[i].PointerToRawData;
        }

        return 0;
    }

public:
    
    /**
    * @brief Loads a PE file from disk.
    *
    * @param file Path to the PE file to load.
    * @return true if file was successfully loaded, false otherwise.
    */
    [[nodiscard]] bool LoadFile( 
        const fs::path & file
    ) noexcept
    {
        std::ifstream in( file, std::ios::binary | std::ios::ate );
        if (!in)
        {
            std::cerr << std::format( "[ERROR] Unable to open '{}'.", file.string() ) << std::endl;
            return false;
        }

        const auto size = static_cast<std::size_t>(in.tellg());
        m_FileData.resize( size );

        in.seekg( 0, std::ios::beg );
        in.read( reinterpret_cast<char *>(m_FileData.data()), m_FileData.size() );
        return true;
    }
    
    
    /**
    * @brief Validates the loaded PE file and extracts necessary information.
    *
    * @return true if validation successful, false otherwise.
    */
    [[nodiscard]] bool ValidatePe() noexcept
    {
        if (!IsValidPe())
        {
            std::cerr << "[ERROR] Not a valid PE file.";
            return false;
        }

        if (!GetImageBase())
        {
            std::cerr << "[ERROR] Unable to get image base." << std::endl;
            return false;
        }

        return true;
    }
    
    
    /**
     * @brief Load patch information from a JSON file.
     *
     * @param json_file Path to the JSON configuration file.
     * @return Map of address strings and the patch information.
     */
    [[nodiscard]] std::map<std::string, PatchInfo> LoadPatches( 
        const fs::path & json_file
    ) const
    {
        std::map<std::string, PatchInfo> patches {};

        // Open and validate JSON file.
        std::ifstream in( json_file );
        if (!in)
        {
            std::cerr << std::format( "[ERROR] Cannot open '{}'.", json_file.string() ) << std::endl;
            return patches;
        }

        json j;

        try
        {
            in >> j;
        }
        catch (const json::parse_error & e)
        {
            std::cerr << std::format( "[ERROR] JSON parse error in '{}': '{}'.", json_file.string(), e.what() ) << std::endl;
            return patches;
        }

        // Ensure the root is an object.
        if (!j.is_object())
        {
            std::cerr << "[ERROR] JSON root must be an object." << std::endl;
            return patches;
        }

        // Safe check for 'Init' field.
        if (j.contains( "Init" ) && j["Init"].is_string())
        {
            const std::string init_addr = j["Init"].get<std::string>();
            patches.try_emplace( init_addr, PatchInfo { init_addr, 0, {} } );

            std::cout << std::format( "[SUCCESS] Loaded CEG init patch at '{}'.", init_addr ) << std::endl;
        }

        // Safe check for 'Terminate' field
        if (j.contains( "Terminate" ) && j["Terminate"].is_string())
        {
            const std::string term_addr = j["Terminate"].get<std::string>();
            patches.try_emplace( term_addr, PatchInfo { term_addr, 0, {} } );

            std::cout << std::format( "[SUCCESS] Loaded CEG terminate patch at '{}'.", term_addr ) << std::endl;
        }

        // Lambda to parse a group of patches with detailed configuration.
        const auto parse_group = [&]( const json & group ) -> void
        {
            if (!group.is_object())
            {
                std::cerr << "[WARNING] Patch group is not an object, skipping." << std::endl;
                return;
            }

            for (const auto & [address, data] : group.items())
            {
                if (!data.is_object())
                {
                    std::cerr << std::format( "[WARNING] Patch data for '{}' is not an object, skipping.", address ) << std::endl;
                    continue;
                }

                PatchInfo info {};

                // Extract the CEG function prologue address.
                if (data.contains( "Prologue" ) && data["Prologue"].is_string())
                    info.m_Prologue = data["Prologue"].get<std::string>();
                else
                {
                    std::cerr << std::format( "[WARNING] Missing or invalid 'Prologue' for patch '{}'.", address ) << std::endl;
                    continue;
                }

                // Extract patch type.
                if (data.contains( "Type" ) && data["Type"].is_number_integer())
                {
                    info.m_Type = data["Type"].get<int>();

                    // Validate patch type.
                    if (info.m_Type < 0 || info.m_Type > 4)
                    {
                        std::cerr << std::format( "[WARNING] Invalid patch type '{}' for address '{}'. Valid types: '0', '1', '2', '3', '4'.",
                            info.m_Type, address ) << std::endl;
                        continue;
                    }
                }

                // Extract the value.
                if (data.contains( "Value" ) && data["Value"].is_string())
                {
                    info.m_Value = data["Value"].get<std::string>();
                }

                // Only add patch if prologue is valid.
                if (!info.m_Prologue.empty())
                {
                    std::cout << std::format( "[SUCCESS] Loaded patch at '{}' (CEG function type: '{}').",
                        address, info.m_Type ) << std::endl;

                    patches.try_emplace( address, std::move( info ) );
                }
            }
        };

        // Load 'ConstantOrStolen' CEG patches.
        if (j.contains( "ConstantOrStolen" ))
        {
            if (j["ConstantOrStolen"].is_array())
            {
                for (const auto & group : j["ConstantOrStolen"])
                    parse_group( group );
            }
            else
                std::cerr << "[WARNING] 'ConstantOrStolen' field exists but is not an array." << std::endl;
        }

        // Lambda to parse array based patches ('Integrity' and 'TestSecret').
        const auto parse_simple_array = [&]( std::string_view key, std::string_view description ) -> void
        {
            if (j.contains( key ))
            {
                if (j[key].is_array())
                {
                    std::size_t count = 0;
                    for (const auto & addr : j[key])
                    {
                        if (addr.is_string())
                        {
                            const std::string addr_str = addr.get<std::string>();
                            patches.try_emplace( addr_str, PatchInfo { addr_str, 0, {} } );
                            ++count;
                        }
                    }

                    std::cout << std::format( "[SUCCESS] Loaded '{}' '{}' patches.", count, description ) << std::endl;
                }
                else
                    std::cerr << std::format( "[WARNING] '{}' field exists but is not an array.", key ) << std::endl;
            }
        };

        // Load 'TestSecret' patches.
        parse_simple_array( "TestSecret", "CEG TestSecret functions." );

        // Load 'Integrity' patches.  
        parse_simple_array( "Integrity", "CEG integrity functions." );

        std::cout << std::format( "[SUCCESS] Total patches loaded: '{}'.", patches.size() ) << std::endl;
        return patches;
    }


    /**
     * @brief Applies the loaded CEG patches to the file buffer.
     *
     * Supports different patch types:
     * 
     * '0' - mov al, 1
     * '1', '2', '3' - Return fixed value (mov eax, <value>)
     * '4' - Jump to the real address (jmp <address>)
     *
     * @param patches Map of patches to apply.
     * @return true if at least one patch was applied, false otherwise.
     */
    [[nodiscard]] bool ApplyPatches( 
        const std::map<std::string, PatchInfo> & patches
    ) noexcept
    {
        // The counter for applied patches.
        std::size_t num_applied { 0 };

        for (const auto & [address, info] : patches)
        {
            const auto prologue = StringToNumber( info.m_Prologue );
            if (prologue < m_ImageBase) 
                continue;

            const auto rva = static_cast<std::uint32_t>( prologue - m_ImageBase );
            const auto offset = RvaToOffset( rva );

            if (offset == 0 || offset >= m_FileData.size() - 5)
                continue;

            try
            {
                switch (info.m_Type)
                {
                    case 0:
                    {
                        m_FileData[offset] = 0xB0;
                        m_FileData[offset + 1] = 0x01;
                        m_FileData[offset + 2] = 0xC3;

                        ++num_applied;
                        break;
                    }
             
                    case 1:
                    case 2:
                    case 3:
                    {
                        const auto val = static_cast<std::uint32_t>(StringToNumber( info.m_Value ));

                        m_FileData[offset] = 0xB8;
                        std::memcpy( &m_FileData[offset + 1], &val, sizeof( val ) );
                        m_FileData[offset + 5] = 0xC3;

                        ++num_applied;
                        break;
                    }

                    case 4:
                    {
                        const auto dest = static_cast<std::uint32_t>(StringToNumber( info.m_Value ));

                        m_FileData[offset] = 0xE9;
                        const auto rel = static_cast<std::int32_t>(dest - (prologue + 5));
                        std::memcpy( &m_FileData[offset + 1], &rel, sizeof( rel ) );

                        ++num_applied;
                        break;
                    }
                }
            }
            catch (const std::exception & e)
            {
                std::cerr << std::format( "[ERROR] Exception during the patch process for '{}' ('{}').", address, e.what() ) << std::endl;
            }
            catch (...)
            {
                std::cerr << std::format( "[ERROR] Exception during the patch process for '{}'.", address ) << std::endl;
            }
        }

        std::cout << std::format( "[SUCCESS] Total patches applied '{}'.", num_applied ) << std::endl;
        return num_applied != 0;
    }
    
    
    /**
    * @brief Saves the patched CEG binary to disk.
    *
    * @param original Path to the original CEG protected binary.
    * @return true if file was successfully saved, false otherwise.
    */
    [[nodiscard]] bool SavePatchedFile( 
        const fs::path & original
    ) const noexcept
    {
        const auto patched = original.stem().string() + "_noceg" + original.extension().string();
        std::ofstream out( patched, std::ios::binary );

        if (!out)
        {
            std::cerr << std::format( "[ERROR] Cannot create '{}'.", patched ) << std::endl;
            return false;
        }

        out.write( reinterpret_cast<const char *>(m_FileData.data()), m_FileData.size() );
        if (out.fail())
        {
            std::cerr << std::format( "[ERROR] Cannot write the patched file '{}'.", patched ) << std::endl;
            return false;
        }

        std::cout << std::format( "[SUCCESS] Saved the patched file as '{}'.", patched ) << std::endl;
        return true;
    }
};


int main( 
    int argc,
    char * argv[]
)
{
    std::cout << "CEG patcher by iArtorias (https://github.com/iArtorias)." << std::endl << std::endl;

    if (argc != 2)
    {
        std::cerr << std::format( "Usage: '{}' <ceg_binary>.", argv[0] ) << std::endl;
        std::cin.get();
        return 1;
    }

    const fs::path ceg_binary = argv[1];
    const fs::path json = fs::current_path() / "noceg.json";

    auto patcher = std::make_unique<Patcher>();

    if (!patcher->LoadFile( ceg_binary ) || !patcher->ValidatePe())
    {
        std::cin.get();
        return 1;
    }

    const auto patches = patcher->LoadPatches( json );
    if (patches.empty())
    {
        std::cerr << std::format( "[ERROR] No patches found in '{}'.", json.string() ) << std::endl;
        std::cin.get();
        return 1;
    }

    if (!patcher->ApplyPatches( patches ))
    {
        std::cerr << "[ERROR] No patches applied." << std::endl;
        std::cin.get();
        return 1;
    }

    if (!patcher->SavePatchedFile( ceg_binary ))
    {
        std::cin.get();
        return 1;
    }

    std::cout << std::endl << "Press 'ENTER' key to exit application." << std::endl;
    std::cin.get();
    return 0;
}