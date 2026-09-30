#pragma once

#include <glm/vec3.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fh::spectators {

/// `media/spectators/spectators.xml`: the people of the crowd, how they are
/// grouped, which animations each skeleton plays and the crowd classes that
/// placements refer to by number. The layout, as the disc ships it
/// (verified):
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
/// A model's mesh is `models/<name><skeleton suffix>.skinbin` and its skin
/// `textures/<texture>.dds.xds`; an animation is `anims/<filename>.anim.bin`
/// (the skeleton's subdir is not part of the path; the game's loader joins
/// "\spectators\anims\" and ".anim.bin" around the file name, verified in the
/// executable). Commented-out entries are ignored by the XML parser, as by
/// the game.
struct SpectatorConfig {
    struct Model {
        std::string name;
        /// Texture name, e.g. "Female01"; several models can share a mesh
        /// shape but not a texture.
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
        /// The `id` attribute, counting from 1; placements store it minus
        /// one (see PlacedSpectator::crowdClass).
        int id = 0;
        std::string name;
        std::string skeleton;
        std::string modelSet;
        /// Played while the crowd is calm, and the first choice of looping
        /// classes, which list nothing else.
        std::vector<WeightedAnimation> idles;
        /// Played when the crowd gets excited (cars passing, a finish).
        std::vector<WeightedAnimation> cheers;
        /// `randomsize`; absent on most classes, where the game's parser
        /// gives true (its default argument at 0x8320E4F8).
        std::optional<bool> randomSize;
        /// `offsetX/Y/Z`, metres; zero wherever the disc sets them.
        glm::vec3 offset {0.0F};
        /// `BBoffsetY`: -1 on the classes that sit on things (shoulders,
        /// walls); by its name, a vertical offset of the billboard sprite,
        /// in metres. Inferred.
        float billboardOffsetY = 0.0F;
    };

    int version = 0;
    /// `numSpriteAngles` (10 on the disc); the atlas holds 9 cells per model,
    /// so the value does not describe the atlas layout.
    int spriteAngles = 0;
    /// `bSpritesBothSides` ("False" on the disc): the atlas holds one side
    /// view and the other is drawn mirrored.
    bool spritesBothSides = false;
    std::vector<Model> models;
    std::vector<ModelSet> modelSets;
    std::vector<Skeleton> skeletons;
    std::vector<CrowdClass> classes;

    /// Parses the file's text. Throws DataError naming `source` if it is not
    /// well-formed XML or has no `spectators` root.
    static SpectatorConfig parse(std::string_view text, const std::string& source);

    /// Lookups by name, ignoring case; -1 when absent.
    int findModel(std::string_view name) const;
    int findModelSet(std::string_view name) const;
    int findSkeleton(std::string_view name) const;
};

} // namespace fh::spectators
