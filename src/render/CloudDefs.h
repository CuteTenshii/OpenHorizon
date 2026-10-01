#pragma once

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openhorizon {
class Vfs;
} // namespace openhorizon

namespace openhorizon::render {

/// `media/realtimesky/clouddefs.xml`: cloud sets (a texture index per layer)
/// and transitions (seconds per layer). Verified: readers at 0x82E36D70
/// (`cloudTransition`) and 0x82E34388 (`cloudSet`), file names from the
/// format strings at 0x8224D49C-0x8224D570.
///
/// The disc has one set, so set selection and transitions (fade state
/// machines at 0x8241D808, 0x8249B380) are not implemented.
struct CloudDefinitions {
    enum class Layer : std::uint8_t {
        CloseFront = 0,
        CloseBack = 1,
        FarFront = 2,
        FarBack = 3,
    };

    struct TransitionLayer {
        Layer layer = Layer::CloseFront;
        float seconds = 0.0F;
    };
    struct Transition {
        std::string name;
        std::vector<TransitionLayer> layers;
    };
    struct Set {
        std::string name;
        bool overcast = false;
        /// Texture index of each layer, indexed by Layer.
        std::array<int, 4> textures{-1, -1, -1, -1};

        /// As the reader decides it: every index at least zero.
        bool valid() const;
        int texture(Layer layer) const { return textures[static_cast<std::size_t>(layer)]; }
    };

    std::vector<Transition> transitions;
    std::vector<Set> sets;

    /// Parses the file's text; throws DataError if it is not a
    /// `cloudDefinitions` document or names an unknown layer.
    static CloudDefinitions parse(std::string_view xml);
    /// Reads `media/realtimesky/clouddefs.xml`; throws DataError if it is
    /// missing or malformed.
    static CloudDefinitions load(const Vfs& vfs);

    const Set* findSet(std::string_view name) const;
};

/// Cloud texture paths as the loaders at 0x82E35680 (close) and 0x82E35BA0
/// (far) name them; `suffix` is "Front", "Back" or "FrontSmall".
std::string farCloudMapPath(std::string_view suffix, int index);
std::string farCloudMaskPath();
std::string closeCloudAlphaDensityPath(std::string_view suffix, int index);
std::string closeCloudTopBottomAmbientPath(std::string_view suffix, int index);
std::string closeCloudAmbientPath(std::string_view suffix, int index);

/// A far cloud layer's time-of-day values (parameter block offsets 352 and
/// 432); defaults from the block's constructor (0x82655D30).
struct FarCloudLayerParameters {
    float speedX = 9.0F;
    float speedZ = 9.0F;
    float coverage = 1.0F;
    glm::vec3 denseColour{0.49F};
    glm::vec3 thinColour{0.64F};
    float denseIntensity = 1.0F;
    float thinIntensity = 1.0F;
    float centreAlpha = 0.0F;
    float edgeAlpha = 0.0F;
    float alphaPower = 5.0F;
};

/// The close cloud layer's values (block offset 128); defaults from
/// 0x82655838.
struct CloseCloudParameters {
    glm::vec3 denseColour{0.0F};
    glm::vec3 thinColour{0.5F};
    glm::vec3 denseColourBack{0.0F};
    glm::vec3 thinColourBack{0.5F};
    glm::vec3 topColour{0.0F};
    glm::vec3 bottomColour{0.0F};
    glm::vec3 bottomColourBack{0.0F};
    glm::vec3 backColour{0.0F};
    float denseColourIntensity = 1.0F;
    float thinColourIntensity = 1.0F;
    float denseColourBackIntensity = 1.0F;
    float thinColourBackIntensity = 1.0F;
    float topPower = 1.0F;
    float topIntensity = 1.0F;
    float bottomPower = 1.0F;
    float bottomIntensity = 1.0F;
    float bottomPowerBack = 1.0F;
    float bottomIntensityBack = 1.0F;
    float backPower = 1.0F;
    float backIntensity = 1.0F;
    float topMaskPower = 1.0F;
    float bottomMaskPower = 1.0F;
    float frontAlpha = 1.0F;
    float backAlpha = 1.0F;
    float topAlpha = 1.0F;
    float bottomAlpha = 0.0F;
    float alphaCurve = 13.01F;
    /// Turns of the layer's texture around the sky per hour of elapsed time.
    float frontRevs = 0.0F;
    float backRevs = 4.0F;
    /// No curve writes these (block offsets 340, 344): always 1.
    glm::vec2 uvScales{1.0F};
};

/// Every cloud value the time of day gives. Verified: curve table at
/// 0x82660E80, sampler 0x8265A428 (clouds at 0x8265B368-0x8265BA48) into the
/// block 0x82656C08 makes, copied to the cloud objects by 0x82E355C8.
struct CloudParameters {
    FarCloudLayerParameters farFront;
    FarCloudLayerParameters farBack;
    CloseCloudParameters close;
};

/// A curve's value by name, or nothing when the track has no keys for it.
using CloudCurveLookup = std::function<std::optional<glm::vec3>(std::string_view name)>;

/// Curves the lookup finds replace the defaults; colours take all three
/// values, the rest the first.
CloudParameters sampleCloudParameters(const CloudCurveLookup& curve);

/// The far layers' texture offsets and the close layer's turn. Verified:
/// far update 0x82418F00 moves masks by speed * elapsed / 3600 (0x82416058,
/// 0x82000D7C), the map 0.4x that (0x82E37EE8); close update 0x824A2B18
/// turns by revs * elapsed / 3600 from 0.5 and 0 (0x82E37E20); all wrap to
/// [0, 1]. Inferred: the elapsed time (0x824A2C08) is real, not game clock.
struct CloudMotion {
    struct FarLayer {
        glm::vec2 map{0.0F};
        glm::vec2 mask1{0.0F};
        glm::vec2 mask2{0.0F};
    };
    FarLayer farFront;
    FarLayer farBack;
    float closeFront = 0.5F;
    float closeBack = 0.0F;

    void advance(const CloudParameters& parameters, float seconds);
};

/// The cloud shaders' lowest vertex height in metres (GainAndMinSkyHeight.y,
/// c100, set at 0x824740F8): 300 sin(atan2(min(start, -1000), 15000)) + 1
/// for FogSkyHeightFadeStart. Verified (atan2 0x82423E40, table 0x83382A80).
float cloudMinSkyHeight(float fogSkyHeightFadeStart);

/// One vertex of the far cloud dome: a position and the texture coordinates
/// whose z is the vertex's row weight.
struct FarCloudVertex {
    glm::vec3 position{0.0F};
    glm::vec3 texcoord{0.0F};
};

/// One vertex of the close cloud dome: a position whose w is the row
/// height, and an inward normal whose w is the texture's u.
struct CloseCloudVertex {
    glm::vec4 position{0.0F};
    glm::vec4 normal{0.0F};
};

template <typename Vertex> struct CloudDome {
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
};

/// The far cloud dome as 0x82E35D08 builds it: a 10000 m sphere centred
/// 9700 m below the camera, cut 6 degrees below the horizon.
CloudDome<FarCloudVertex> buildFarCloudDome();

/// The close cloud dome as 0x82E35830 builds it: a 300 m sphere around the
/// camera, rings from -6 to 84 degrees.
CloudDome<CloseCloudVertex> buildCloseCloudDome();

} // namespace openhorizon::render
