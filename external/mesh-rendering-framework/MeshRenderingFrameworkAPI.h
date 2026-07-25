#pragma once

#define ENABLE_MENU_FRAMEWORK

#ifdef ENABLE_MENU_FRAMEWORK
    #include "SKSEMenuFramework.h"
#endif

namespace MeshRenderingFrameworkAPI {

    namespace Internal {
        class IMesh {
        public:
            uint64_t id;
            RE::NiMatrix3 rotation;
            RE::NiPoint3 position;
            RE::NiPoint3 boundMin;
            RE::NiPoint3 boundMax;
            float scale = 1;
            uint32_t width;
            uint32_t height;
            ID3D11Texture2D* texture = nullptr;
            ID3D11ShaderResourceView* SRV = nullptr;
            bool saveNextFrame = false;
            bool deleteAfterSave = false;
            const char* savePath = nullptr;
            bool mustUpdate = true;
            bool alwaysUpdate = false;
        };

        template <class T>
        T GetFunction(LPCSTR name) {
            static auto meshRenderer = GetModuleHandle(L"MeshRenderingFramework");
            if (!meshRenderer) {
                return nullptr;
            }
            return reinterpret_cast<T>(GetProcAddress(meshRenderer, name));
        }

        inline IMesh* __stdcall IMesh_CreateByNifPath(const char* nifPath, uint32_t width, uint32_t height) {
            auto function = GetFunction<decltype(&IMesh_CreateByNifPath)>("IMesh_CreateByNifPath");
            if (!function) {
                return nullptr;
            }
            return function(nifPath, width, height);
        }

        inline IMesh* __stdcall IMesh_CreateByNiAVObjectList(RE::NiAVObject* const* objects, uint32_t objectCount, uint32_t width, uint32_t height) {
            auto function = GetFunction<decltype(&IMesh_CreateByNiAVObjectList)>("IMesh_CreateByNiAVObjectList");
            if (!function) {
                return nullptr;
            }
            return function(objects, objectCount, width, height);
        }

        inline void __stdcall IMesh_Delete(IMesh* mesh) {
            auto function = GetFunction<decltype(&IMesh_Delete)>("IMesh_Delete");
            if (!function) {
                return;
            }
            return function(mesh);
        }

        inline IMesh* __stdcall IMesh_Save(IMesh* mesh, const char* filePath) {
            auto function = GetFunction<decltype(&IMesh_Save)>("IMesh_Save");
            if (!function) {
                return nullptr;
            }
            return function(mesh, filePath);
        }

        inline const char* GetModelPathFromBaseObject(RE::TESBoundObject* base) {
            if (!base) {
                return nullptr;
            }

            if (auto weapon = base->As<RE::TESObjectWEAP>()) {
                if (auto first = weapon->firstPersonModelObject) {
                    auto path = first->GetModel();
                    if (path && path[0]) {
                        return path;
                    }
                }
            }

            if (auto npc = base->As<RE::TESNPC>()) {
                auto sex = npc->GetSex();
                auto race = npc->GetRace();

                auto getPathFromArmor = [&](RE::TESObjectARMO* armor) -> const char* {
                    if (!armor || !race) {
                        return nullptr;
                    }

                    if (auto arma = armor->GetArmorAddon(race)) {
                        auto path = arma->bipedModels[sex].GetModel();
                        if (path && path[0]) {
                            return path;
                        }
                    }

                    return nullptr;
                };

                if (auto path = getPathFromArmor(npc->skin)) {
                    return path;
                }

                if (race) {
                    if (auto path = getPathFromArmor(race->skin)) {
                        return path;
                    }
                }

                if (auto path = getPathFromArmor(npc->farSkin)) {
                    return path;
                }

                for (std::int8_t i = 0; i < npc->numHeadParts; ++i) {
                    if (auto headPart = npc->headParts[i]) {
                        auto path = headPart->GetModel();
                        if (path && path[0]) {
                            return path;
                        }
                    }
                }
            }

            auto swap = base->As<RE::TESModel>();

            if (swap) {
                auto path = swap->GetModel();
                if (path && path[0]) {
                    return path;
                }
            }
            if (auto biped = base->As<RE::TESBipedModelForm>()) {
                auto player = RE::PlayerCharacter::GetSingleton();
                if (!player) {
                    return nullptr;
                }

                auto playerBase = player->GetActorBase();
                if (!playerBase) {
                    return nullptr;
                }

                auto sex = playerBase->GetSex();
                auto& worldModel = biped->worldModels[sex];
                auto path = worldModel.GetModel();
                if (path && path[0]) {
                    return path;
                }
            }

            if (auto spell = base->As<RE::SpellItem>()) {
                if (auto obj = spell->GetMenuDisplayObject()) {
                    if (auto model = obj->As<RE::TESModel>()) {
                        auto path = model->GetModel();
                        if (path && path[0]) {
                            return path;
                        }
                    }
                }
            }

            return nullptr;
        }

        inline IMesh* CreateFromBaseObject(RE::TESBoundObject* base, uint32_t width, uint32_t height) {
            auto path = GetModelPathFromBaseObject(base);
            if (!path || !path[0]) {
                return nullptr;
            }

            return IMesh_CreateByNifPath(path, width, height);
        }

        inline IMesh* CreateFromNiAVObjectList(RE::NiAVObject* const* objects, uint32_t objectCount, uint32_t width, uint32_t height) {
            if (!objects || objectCount == 0) {
                return nullptr;
            }

            return IMesh_CreateByNiAVObjectList(objects, objectCount, width, height);
        }

    }

    inline bool HasRenderableModel(RE::TESBoundObject* base) {
        return Internal::GetModelPathFromBaseObject(base) != nullptr;
    }

    class Mesh {
    protected:
        Internal::IMesh* mesh;
        RE::TESBoundObject* base;

    public:
        static void Render(const char* filePath, RE::TESBoundObject* base, uint32_t width, uint32_t height) {
            std::filesystem::path path(filePath);
            if (std::filesystem::exists(path)) {
                return;
            }
            auto mesh = new Mesh(base, width, height);
            mesh->Save(filePath);
            if (mesh->mesh) {
                mesh->mesh->deleteAfterSave = true;
            }
        }
        static void Render(const char* filePath, RE::FormID id, uint32_t width, uint32_t height) {
            std::filesystem::path path(filePath);
            if (std::filesystem::exists(path)) {
                return;
            }
            auto mesh = new Mesh(id, width, height);
            mesh->Save(filePath);
            if (mesh->mesh) {
                mesh->mesh->deleteAfterSave = true;
            }
        }
        static void Render(const char* filePath, const char* nifPath, uint32_t width, uint32_t height) {
            std::filesystem::path path(filePath);
            if (std::filesystem::exists(path)) {
                return;
            }
            auto mesh = new Mesh(nifPath, width, height);
            mesh->Save(filePath);
            if (mesh->mesh) {
                mesh->mesh->deleteAfterSave = true;
            }
        }
        void Save(const char* filePath) {
            if (!mesh) {
                return;
            }
            if (mesh->SRV) {
                Internal::IMesh_Save(mesh, filePath);
            } else {
                mesh->savePath = strdup(filePath);
                mesh->saveNextFrame = true;
            }
        }
        ID3D11ShaderResourceView* GetResourceView() {
            if (!mesh) {
                return nullptr;
            }
            return mesh->SRV;
        }
        RE::TESBoundObject* GetBase() { return base; }
        RE::NiPoint3 GetPosition() const {
            return mesh ? mesh->position : RE::NiPoint3{};
        }
        void SetRotation(RE::NiMatrix3 rotation) {
            if (!mesh) {
                return;
            }
            mesh->rotation = rotation;
            mesh->mustUpdate = true;
        }
        void SetPosition(RE::NiPoint3 position) {
            if (!mesh) {
                return;
            }
            mesh->position = position;
            mesh->mustUpdate = true;
        }
        void ScaleUp(float scale) {
            if (!mesh) {
                return;
            }
            mesh->scale *= scale;
            mesh->mustUpdate = true;
        }
        void SetAlwaysUpdate(bool value) {
            if (!mesh) {
                return;
            }
            mesh->alwaysUpdate = value;
        }
        bool IsValid() const { return mesh != nullptr; }
        ~Mesh() {
            if (mesh) {
                Internal::IMesh_Delete(mesh);
            }
        }

        Mesh(RE::TESBoundObject* base, uint32_t width, uint32_t height) {
            this->base = base;
            mesh = Internal::CreateFromBaseObject(base, width, height);
        }

        Mesh(RE::FormID id, uint32_t width, uint32_t height) {
            base = RE::TESForm::LookupByID<RE::TESBoundObject>(id);
            if (!base) {
                mesh = nullptr;
                return;
            }
            mesh = Internal::CreateFromBaseObject(base, width, height);
        }
        Mesh(const char* path, uint32_t width, uint32_t height) {
            base = nullptr;
            mesh = Internal::IMesh_CreateByNifPath(path, width, height);
        }
        Mesh(RE::NiAVObject* const* objects, uint32_t objectCount, uint32_t width, uint32_t height) {
            base = nullptr;
            mesh = Internal::CreateFromNiAVObjectList(objects, objectCount, width, height);
        }
    };

#ifdef ENABLE_MENU_FRAMEWORK
    class OrbitMesh : public Mesh {
        RE::NiMatrix3 orientation;
        bool orientationChanged = true;

    public:
        void SetOrbitOrientation(RE::NiMatrix3 orientation) {
            if (!mesh) {
                return;
            }

            this->orientation = orientation;
            orientationChanged = true;
        }

        OrbitMesh(RE::TESBoundObject* base, uint32_t width, uint32_t height) : Mesh(base, width, height) { ScaleUp(0.8f); }

        OrbitMesh(RE::FormID id, uint32_t width, uint32_t height) : Mesh(id, width, height) { ScaleUp(0.8f); }

        OrbitMesh(const char* path, uint32_t width, uint32_t height) : Mesh(path, width, height) { ScaleUp(0.8f); }

        OrbitMesh(RE::NiAVObject* const* objects, uint32_t objectCount, uint32_t width, uint32_t height) : Mesh(objects, objectCount, width, height) { ScaleUp(0.8f); }

        void Render(const char* name) {
            if (GetResourceView()) {
                const ImGuiMCP::ImVec2 imageSize{mesh->width, mesh->height};

                ImGuiMCP::InvisibleButton(name, imageSize);

                const auto imageMin = ImGuiMCP::GetItemRectMin();
                const auto imageMax = ImGuiMCP::GetItemRectMax();

                ImGuiMCP::ImDrawListManager::AddImage(ImGuiMCP::GetWindowDrawList(), (ImGuiMCP::ImTextureID)GetResourceView(), imageMin, imageMax, {0, 0}, {1, 1}, IM_COL32_WHITE);

                if (ImGuiMCP::IsItemActive() && ImGuiMCP::IsMouseDown(ImGuiMCP::ImGuiMouseButton_Left)) {
                    const auto delta = ImGuiMCP::GetIO()->MouseDelta;

                    if (delta.x != 0.0f || delta.y != 0.0f) {
                        const auto camera = RE::UI3DSceneManager::GetSingleton()->camera;

                        RE::NiMatrix3 yaw;
                        yaw.SetEulerAnglesXYZ(0.0f, -delta.x * 0.01f, 0.0f);

                        RE::NiMatrix3 pitch;
                        pitch.SetEulerAnglesXYZ(0.0f, 0.0f, -delta.y * 0.01f);

                        const auto cameraRotation = camera->world.rotate;
                        const auto cameraDelta = cameraRotation * pitch * yaw * cameraRotation.Transpose();

                        orientation = cameraDelta * orientation;
                        orientationChanged = true;
                    }
                }

                if (orientationChanged) {
                    SetRotation(orientation);
                    orientationChanged = false;
                }
            }
        }
    };
#endif
}
