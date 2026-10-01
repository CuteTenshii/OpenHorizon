#pragma once

#include "base/Bytes.h"
#include "xenos/Effect.h"
#include "xenos/ShaderProgram.h"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openhorizon {
class Vfs;
} // namespace openhorizon

namespace openhorizon::carmodel {

/// One pass of a car technique, translated.
struct CarPass {
    xenos::ShaderProgramSource program;
    /// Effect path, technique and pass index: the same translation across reloads of the effect.
    std::string identity;
    /// The pixel shader writes the square root of its colour to target 0
    /// (the effects' `sqrt oC0` ending). The game's scene target keeps it so
    /// and its post-processing squares what it reads (0x821C8238), so a
    /// renderer with a linear target squares the output instead.
    bool squareRootOutput = false;
    /// Blending the pass sets itself (render states 60, 72 and 76 of its
    /// state list: enable, source and destination factor as Xbox 360
    /// D3DBLEND values, 0 zero, 1 one, 6 source alpha, 7 inverse source
    /// alpha, 8 destination colour). Nothing when the pass leaves blending
    /// to the engine, as most car passes do.
    struct Blend {
        bool enable = false;
        std::uint32_t source = 1;
        std::uint32_t destination = 0;
    };
    std::optional<Blend> blend;
};

/// One effect of `media/shaders/cars`, with its passes translated to GLSL on
/// first use. Shared by every car and safe to use from any thread.
class CarEffect {
public:
    /// The effect at `path`, parsed once per process. Throws DataError when
    /// it is missing or does not parse.
    static std::shared_ptr<const CarEffect> load(const Vfs& vfs, const std::string& path);

    const std::string& path() const { return m_path; }
    const xenos::Effect& effect() const { return m_effect; }
    bool hasTechnique(std::string_view technique) const;

    /// The passes of `technique` that run shaders (the effects end some
    /// techniques with a `Cleanup` pass that only restores states), in
    /// order. Empty when there is no such technique or a pass does not
    /// translate; the reason is kept for error().
    std::vector<std::shared_ptr<const CarPass>> passes(std::string_view technique) const;
    /// Why passes() came back empty for `technique`, or an empty string.
    std::string error(std::string_view technique) const;

    CarEffect(std::string path, Bytes data, xenos::Effect effect);

private:
    struct Entry {
        std::vector<std::shared_ptr<const CarPass>> passes;
        std::string error;
    };

    std::string m_path;
    Bytes m_data;
    xenos::Effect m_effect;
    mutable std::mutex m_mutex;
    mutable std::map<std::string, Entry, std::less<>> m_translated;
};

} // namespace openhorizon::carmodel
