#pragma once

#include <glm/vec3.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openhorizon::spectators {

/// `media/spectators/spectators.xml`. Layout, verified:
///
///     <spectators>
///       <models Version numSpriteAngles bSpritesBothSides>
///         <model name spritesPerTex> <texture name/> </model> ...
///       </models>
///       <modelset name> <model name/> ... </modelset> ...
///       <skeleton Version name suffix subdir>
///         <animation name filename/> ... <modelset name/>
///       </skeleton> ...
///       <crowdclass id name [randomsize] [offsetX offsetY offsetZ] [BBoffsetY]>
///         <skeleton name/> <modelset name/>
///         <idle name weight/> ... <cheer name weight/> ...
///       </crowdclass> ...
///     </spectators>
///
/// Meshes are `models/<name><skeleton suffix>.skinbin`, skins `textures/<texture>.dds.xds`, animations
/// `anims/<filename>.anim.bin` without the skeleton's subdir (verified in the executable's loader).
struct SpectatorConfig {
    struct Model {
        std::string name;
        /// E.g. "Female01"; models can share a mesh shape but not a texture.
        std::string texture;
        /// Sprite atlas cells per model (9 on the disc).
        int spritesPerTexture = 0;
    };
    struct ModelSet {
        std::string name;
        std::vector<std::string> models;
    };
    struct Animation {
        std::string name;
        std::string file;
    };
    struct Skeleton {
        std::string name;
        /// Appended to a model name to name its mesh: "_Stand" or "_Sit".
        std::string suffix;
        std::string subdir;
        std::vector<Animation> animations;
        std::string modelSet;
    };
    struct WeightedAnimation {
        std::string name;
        float weight = 1.0F;
    };
    struct CrowdClass {
        /// Counts from 1; placements store it minus one.
        int id = 0;
        std::string name;
        std::string skeleton;
        std::string modelSet;
        /// Played while the crowd is calm; looping classes list nothing else.
        std::vector<WeightedAnimation> idles;
        /// Played when the crowd gets excited (cars passing, a finish).
        std::vector<WeightedAnimation> cheers;
        /// Absent means true, the parser's default (0x8320E4F8).
        std::optional<bool> randomSize;
        /// `offsetX/Y/Z`, metres; zero wherever the disc sets them.
        glm::vec3 offset {0.0F};
        /// -1 on classes that sit on things; inferred to be the sprite's vertical offset in metres.
        float billboardOffsetY = 0.0F;
    };

    int version = 0;
    /// 10 on the disc, yet the atlas holds 9 cells per model: not the atlas layout.
    int spriteAngles = 0;
    /// False on the disc: the other side view is drawn mirrored.
    bool spritesBothSides = false;
    std::vector<Model> models;
    std::vector<ModelSet> modelSets;
    std::vector<Skeleton> skeletons;
    std::vector<CrowdClass> classes;

    /// Throws DataError naming `source` if it is not well-formed XML or has no `spectators` root.
    static SpectatorConfig parse(std::string_view text, const std::string& source);

    /// Lookups by name, ignoring case; -1 when absent.
    int findModel(std::string_view name) const;
    int findModelSet(std::string_view name) const;
    int findSkeleton(std::string_view name) const;
};

} // namespace openhorizon::spectators
