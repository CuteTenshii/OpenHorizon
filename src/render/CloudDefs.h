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

/// `media/realtimesky/clouddefs.xml`: named cloud sets, each giving the
/// texture index of the four cloud layers, and transitions giving the time
/// each layer takes to change from one set to another.
///
/// Verified against the executable: the `cloudTransition` reader at
/// 0x82E36D70 turns each `layer` element's `name` into a layer number by
/// comparing it with "closeCloudFront", "closeCloudBack", "farCloudFront"
/// and "farCloudBack" (0 to 3, in that order) and reads its `time` as a
/// float; the `cloudSet` reader at 0x82E34388 reads the four indices as
/// integers, `name` and `overcast`, and marks the set valid only when all
/// four indices are at least zero. The texture file names come from the
/// format strings at 0x8224D49C-0x8224D570 ("...CloseCloudAmbient%s%d.xds"
/// and so on), `%s` being the layer object's suffix and `%d` its index.
///
/// The disc defines one set, "base", with every index 0, and one
/// transition, "base", of 5 s per layer; only index-0 cloud textures exist
/// in `media/realtimesky`. Missing: how the game picks among several sets,
/// which set index feeds which layer object, and when a transition starts
/// (the per-layer fade state machines at 0x8241D808 and 0x8249B380 run only
/// once a different set is requested). With a single set nothing ever
/// changes, so none of that is implemented.
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

/// The realtime sky's cloud textures, as the loaders at 0x82E35680 (close
/// layers) and 0x82E35BA0 (far layers) name them: `suffix` is the layer
/// object's "Front", "Back" or "FrontSmall".
std::string farCloudMapPath(std::string_view suffix, int index);
std::string farCloudMaskPath();
std::string closeCloudAlphaDensityPath(std::string_view suffix, int index);
std::string closeCloudTopBottomAmbientPath(std::string_view suffix, int index);
std::string closeCloudAmbientPath(std::string_view suffix, int index);

/// One far cloud layer's values from the time-of-day curves, laid out as
/// the sky's parameter block holds them (block offset 352 for the front
/// layer, 432 for the back one; see CloudParameters). The defaults are
/// those the block's constructor writes (0x82655D30), which stand wherever
/// a track's TimeOfDayA.xml has no keys for a curve.
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

/// The close cloud layer's values (block offset 128), with the defaults of
/// the block's constructor (0x82655838).
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
    /// No curve writes these (block offsets 340 and 344); the constructor's
    /// 1.0 is what CloseCloud_VS's UVScales always receives.
    glm::vec2 uvScales{1.0F};
};

/// Every cloud value the time of day gives.
///
/// Verified: the curve table (0x82660E80) registers the 49 cloud curves
/// with one value each or three for colours, and the time-of-day sampler
/// (from 0x8265A428; the cloud curves at 0x8265B368-0x8265BA48) samples each
/// curve that has keys into a block made by 0x82656C08 and leaves the
/// block's default otherwise. CRealtimeSky's parameter setter (0x82E355C8)
/// copies the block's 160 bytes at 352 to the far cloud object and its 224
/// bytes at 128 to the close cloud object, and the draws read them from
/// there (see Clouds).
struct CloudParameters {
    FarCloudLayerParameters farFront;
    FarCloudLayerParameters farBack;
    CloseCloudParameters close;
};

/// A curve's value by name, or nothing when the track has no keys for it.
using CloudCurveLookup = std::function<std::optional<glm::vec3>(std::string_view name)>;

/// Samples the cloud curves as the time-of-day sampler does: the curves
/// the lookup finds replace the defaults, colours taking all three values
/// and the rest the first.
CloudParameters sampleCloudParameters(const CloudCurveLookup& curve);

/// The far layers' texture offsets and the close layer's turn, which the
/// sky advances every update.
///
/// Verified: the far cloud update (0x82418F00) moves each far layer's two
/// mask offsets by `speed * elapsed / 3600` (speed X along u, Z along v;
/// 0x82416058, the factor at 0x82000D7C) and its map offset by 0.4 times
/// that (the far object's field 416, set to 0.4 by its constructor at
/// 0x82E37EE8), wrapping each component into [0, 1]. The close cloud update
/// (0x824A2B18) moves the front and back turns by `revs * elapsed / 3600`
/// with the same wrap; the constructor (0x82E37E20) starts the front at 0.5
/// and the back at 0. Inferred, not traced to its source: that the elapsed
/// time the presentation update passes down (0x824A2C08, from a virtual
/// call) is real time rather than game-clock time.
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

/// The lowest height, in metres of the sky domes, that the cloud vertex
/// shaders let a vertex take (GainAndMinSkyHeight.y, register c100, set
/// for every sky shader at 0x824740F8): `300 sin(atan2(min(start, -1000),
/// 15000)) + 1` for the sky fog's height fade start (the sky object's
/// field 1224, copied from the parameter block at 664 where the sampler
/// puts FogSkyHeightFadeStart). Verified: 0x82423E40 is a two-argument
/// arctangent (its table at 0x83382A80 holds pi/2, pi and tan 15 degrees).
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

/// The far cloud dome as the far cloud object builds it (0x82E35D08):
/// 1501 vertices and 8700 indices. A sphere of radius 10000 m whose centre
/// is 9700 m below the camera, cut where a ray 6 degrees below the horizon
/// meets it: 15 rings of 100 vertices from that cut towards the top, the
/// ring k at latitude `lat0 + (90 degrees - lat0) k / 15`, and one vertex
/// at the top (0, 300, 0). A vertex at azimuth a on ring k has texture
/// coordinates `(sin a, cos a) (1 - k / 15) / 2` and weight `1 - k / 15`.
CloudDome<FarCloudVertex> buildFarCloudDome();

/// The close cloud dome (0x82E35830): 1617 vertices and 9300 indices on a
/// sphere of 300 m around the camera. 16 rings from -6 to 84 degrees of
/// elevation in 6-degree steps, each of 101 vertices (the first repeated
/// at the end with u = 1), with w = (ring + 1) / 15; the top vertex has
/// w = 1 and u = 0.5.
CloudDome<CloseCloudVertex> buildCloseCloudDome();

} // namespace openhorizon::render
