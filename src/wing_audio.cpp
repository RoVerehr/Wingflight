#include "wing_audio.hpp"
#include "diagnostics.hpp"
#include <RE/I/ID.h>
#include <atomic>
namespace wingflight {
bool buildWingSound(RE::BSSoundHandle& handle,RE::BGSSoundDescriptorForm* descriptor,
    const std::vector<std::string>& files,std::uint32_t outputModel){
    const auto audio=RE::BSAudioManager::GetSingleton();if(!audio)return false;
    if(files.empty())return descriptor&&audio->GetSoundHandle(handle,descriptor);
    static std::atomic_uint sequence{};
    const auto& path=files[sequence.fetch_add(1,std::memory_order_relaxed)%files.size()];
    RE::BSResource::ID file{};file.GenerateFromPath(path.c_str());
    audio->GetSoundHandleByFile(handle,file,0x1A,128);
    if(!handle.IsValid()){
        diagnostic::report("sound.file-handle-invalid","skip unavailable wing audio file",nullptr,path);return false;
    }
    // Use native positional attenuation, as the original wing sound records do.
    if(auto output=RE::TESDataHandler::GetSingleton()->LookupForm<RE::BGSSoundOutput>(outputModel,"Skyrim.esm"))handle.SetOutputModel(output);
    return true;
}
}
