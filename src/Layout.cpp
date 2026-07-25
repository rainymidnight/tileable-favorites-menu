#include "Layout.h"

namespace TFM
{
    namespace
    {
        constexpr std::size_t kMaximumDepth = 64;

        template <class T>
        bool WriteValue(SKSE::SerializationInterface* serialization, const T& value)
        {
            return serialization->WriteRecordData(std::addressof(value), sizeof(value));
        }

        template <class T>
        bool ReadValue(SKSE::SerializationInterface* serialization, T& value)
        {
            return serialization->ReadRecordData(std::addressof(value), sizeof(value)) == sizeof(value);
        }

        void CollectKeys(const LayoutNode* node, std::unordered_set<ItemKey, ItemKeyHash>& keys)
        {
            if (!node) {
                return;
            }
            if (node->IsLeaf()) {
                if (node->item.IsValid()) {
                    keys.insert(node->item);
                }
                return;
            }
            CollectKeys(node->first.get(), keys);
            CollectKeys(node->second.get(), keys);
        }

        std::unique_ptr<LayoutNode> Prune(
            std::unique_ptr<LayoutNode> node,
            const std::unordered_set<ItemKey, ItemKeyHash>& valid)
        {
            if (!node) {
                return nullptr;
            }
            if (node->IsLeaf()) {
                return valid.contains(node->item) ? std::move(node) : nullptr;
            }

            node->first = Prune(std::move(node->first), valid);
            node->second = Prune(std::move(node->second), valid);
            if (!node->first) {
                return std::move(node->second);
            }
            if (!node->second) {
                return std::move(node->first);
            }
            return node;
        }

        void FindShallowestLeaf(LayoutNode* node, std::size_t depth, LayoutNode*& best, std::size_t& bestDepth)
        {
            if (!node) {
                return;
            }
            if (node->IsLeaf()) {
                if (!best || depth < bestDepth) {
                    best = node;
                    bestDepth = depth;
                }
                return;
            }
            FindShallowestLeaf(node->first.get(), depth + 1, best, bestDepth);
            FindShallowestLeaf(node->second.get(), depth + 1, best, bestDepth);
        }

        void InsertItem(std::unique_ptr<LayoutNode>& root, const ItemKey& item)
        {
            if (!root) {
                root = std::make_unique<LayoutNode>();
                root->item = item;
                return;
            }

            LayoutNode* leaf = nullptr;
            std::size_t bestDepth = 0;
            FindShallowestLeaf(root.get(), 0, leaf, bestDepth);

            auto oldItem = leaf->item;
            leaf->item = {};
            leaf->axis = bestDepth % 2 == 0 ? Axis::kVertical : Axis::kHorizontal;
            leaf->ratio = 0.5f;
            leaf->first = std::make_unique<LayoutNode>();
            leaf->first->item = oldItem;
            leaf->second = std::make_unique<LayoutNode>();
            leaf->second->item = item;
        }

        void CalculateNode(
            const LayoutNode* node,
            const Rect& rect,
            float gap,
            std::vector<LeafRect>& leaves)
        {
            if (!node) {
                return;
            }
            if (node->IsLeaf()) {
                leaves.push_back({ node->item, rect });
                return;
            }

            if (node->axis == Axis::kVertical) {
                const auto usable = std::max(0.0f, rect.width - gap);
                const auto firstWidth = usable * node->ratio;
                const Rect first{ rect.x, rect.y, firstWidth, rect.height };
                const Rect second{ rect.x + firstWidth + gap, rect.y, usable - firstWidth, rect.height };
                CalculateNode(node->first.get(), first, gap, leaves);
                CalculateNode(node->second.get(), second, gap, leaves);
            } else {
                const auto usable = std::max(0.0f, rect.height - gap);
                const auto firstHeight = usable * node->ratio;
                const Rect first{ rect.x, rect.y, rect.width, firstHeight };
                const Rect second{ rect.x, rect.y + firstHeight + gap, rect.width, usable - firstHeight };
                CalculateNode(node->first.get(), first, gap, leaves);
                CalculateNode(node->second.get(), second, gap, leaves);
            }
        }

        LayoutNode* FindLeaf(LayoutNode* node, const ItemKey& key)
        {
            if (!node) {
                return nullptr;
            }
            if (node->IsLeaf()) {
                return node->item == key ? node : nullptr;
            }
            if (auto result = FindLeaf(node->first.get(), key)) {
                return result;
            }
            return FindLeaf(node->second.get(), key);
        }

        std::unique_ptr<LayoutNode> CloneNode(const LayoutNode* node)
        {
            if (!node) {
                return nullptr;
            }
            auto clone = std::make_unique<LayoutNode>();
            clone->item = node->item;
            clone->axis = node->axis;
            clone->ratio = node->ratio;
            clone->first = CloneNode(node->first.get());
            clone->second = CloneNode(node->second.get());
            return clone;
        }

        std::unique_ptr<LayoutNode>* FindLeafSlot(
            std::unique_ptr<LayoutNode>& node,
            const ItemKey& key)
        {
            if (!node) {
                return nullptr;
            }
            if (node->IsLeaf()) {
                return node->item == key ? std::addressof(node) : nullptr;
            }
            if (auto slot = FindLeafSlot(node->first, key)) {
                return slot;
            }
            return FindLeafSlot(node->second, key);
        }

        bool ExtractLeaf(
            std::unique_ptr<LayoutNode>& node,
            const ItemKey& key,
            std::unique_ptr<LayoutNode>& extracted)
        {
            if (!node) {
                return false;
            }
            if (node->IsLeaf()) {
                if (node->item != key) {
                    return false;
                }
                extracted = std::move(node);
                return true;
            }

            if (!ExtractLeaf(node->first, key, extracted) &&
                !ExtractLeaf(node->second, key, extracted)) {
                return false;
            }
            if (!node->first) {
                node = std::move(node->second);
            } else if (!node->second) {
                node = std::move(node->first);
            }
            return true;
        }

        [[nodiscard]] std::size_t TreeDepth(const LayoutNode* node)
        {
            if (!node || node->IsLeaf()) {
                return 0;
            }
            return 1 + std::max(TreeDepth(node->first.get()), TreeDepth(node->second.get()));
        }

        bool ApplyDropToTree(std::unique_ptr<LayoutNode>& root, const LayoutDrop& drop)
        {
            if (!drop.source.IsValid() || !drop.target.IsValid() || drop.source == drop.target) {
                return false;
            }

            if (drop.position == DropPosition::kCenter) {
                const auto source = FindLeaf(root.get(), drop.source);
                const auto target = FindLeaf(root.get(), drop.target);
                if (!source || !target) {
                    return false;
                }
                std::swap(source->item, target->item);
                return true;
            }

            if (!FindLeaf(root.get(), drop.source) || !FindLeaf(root.get(), drop.target)) {
                return false;
            }

            std::unique_ptr<LayoutNode> source;
            if (!ExtractLeaf(root, drop.source, source)) {
                return false;
            }
            const auto targetSlot = FindLeafSlot(root, drop.target);
            if (!targetSlot) {
                return false;
            }

            auto target = std::move(*targetSlot);
            auto split = std::make_unique<LayoutNode>();
            split->axis = drop.position == DropPosition::kLeft || drop.position == DropPosition::kRight ?
                Axis::kVertical : Axis::kHorizontal;
            split->ratio = 0.5f;
            const bool sourceFirst =
                drop.position == DropPosition::kLeft || drop.position == DropPosition::kTop;
            split->first = sourceFirst ? std::move(source) : std::move(target);
            split->second = sourceFirst ? std::move(target) : std::move(source);
            *targetSlot = std::move(split);
            return TreeDepth(root.get()) <= kMaximumDepth;
        }

        bool WriteNode(SKSE::SerializationInterface* serialization, const LayoutNode* node)
        {
            const std::uint8_t kind = node->IsLeaf() ? 0U : 1U;
            if (!WriteValue(serialization, kind)) {
                return false;
            }
            if (node->IsLeaf()) {
                return WriteValue(serialization, node->item.formID) &&
                    WriteValue(serialization, node->item.uniqueID);
            }

            const auto axis = static_cast<std::uint8_t>(node->axis);
            return WriteValue(serialization, axis) && WriteValue(serialization, node->ratio) &&
                WriteNode(serialization, node->first.get()) && WriteNode(serialization, node->second.get());
        }

        std::unique_ptr<LayoutNode> ReadNode(SKSE::SerializationInterface* serialization, std::size_t depth, bool& ok)
        {
            if (!ok || depth > kMaximumDepth) {
                ok = false;
                return nullptr;
            }

            std::uint8_t kind = 0;
            if (!ReadValue(serialization, kind) || kind > 1) {
                ok = false;
                return nullptr;
            }

            auto node = std::make_unique<LayoutNode>();
            if (kind == 0) {
                if (!ReadValue(serialization, node->item.formID) || !ReadValue(serialization, node->item.uniqueID)) {
                    ok = false;
                    return nullptr;
                }
                if (node->item.formID != 0) {
                    RE::FormID resolved = 0;
                    if (!serialization->ResolveFormID(node->item.formID, resolved)) {
                        node->item = {};
                    } else {
                        node->item.formID = resolved;
                    }
                }
                return node;
            }

            std::uint8_t axis = 0;
            if (!ReadValue(serialization, axis) || axis > 1 || !ReadValue(serialization, node->ratio)) {
                ok = false;
                return nullptr;
            }
            node->axis = static_cast<Axis>(axis);
            node->ratio = std::clamp(node->ratio, 0.1f, 0.9f);
            node->first = ReadNode(serialization, depth + 1, ok);
            node->second = ReadNode(serialization, depth + 1, ok);
            if (!ok) {
                return nullptr;
            }
            return node;
        }
    }

    Layout& Layout::GetSingleton()
    {
        static Layout singleton;
        return singleton;
    }

    void Layout::Reconcile(const std::vector<FavoriteItem>& favorites)
    {
        std::scoped_lock lock(mutex_);
        std::unordered_set<ItemKey, ItemKeyHash> valid;
        valid.reserve(favorites.size());
        for (const auto& favorite : favorites) {
            valid.insert(favorite.key);
        }

        root_ = Prune(std::move(root_), valid);
        std::unordered_set<ItemKey, ItemKeyHash> existing;
        CollectKeys(root_.get(), existing);
        for (const auto& favorite : favorites) {
            if (!existing.contains(favorite.key)) {
                InsertItem(root_, favorite.key);
                existing.insert(favorite.key);
            }
        }
    }

    void Layout::Clear()
    {
        std::scoped_lock lock(mutex_);
        root_.reset();
    }

    std::vector<LeafRect> Layout::Calculate(const Rect& bounds, float gap)
    {
        std::scoped_lock lock(mutex_);
        std::vector<LeafRect> result;
        CalculateNode(root_.get(), bounds, gap, result);
        return result;
    }

    std::optional<std::vector<LeafRect>> Layout::CalculatePreview(
        const Rect& bounds,
        float gap,
        const LayoutDrop& drop)
    {
        std::scoped_lock lock(mutex_);
        auto preview = CloneNode(root_.get());
        if (!ApplyDropToTree(preview, drop)) {
            return std::nullopt;
        }
        std::vector<LeafRect> result;
        CalculateNode(preview.get(), bounds, gap, result);
        return result;
    }

    bool Layout::ApplyDrop(const LayoutDrop& drop)
    {
        std::scoped_lock lock(mutex_);
        auto updated = CloneNode(root_.get());
        if (!ApplyDropToTree(updated, drop)) {
            return false;
        }
        root_ = std::move(updated);
        return true;
    }

    bool Layout::Save(SKSE::SerializationInterface* serialization) const
    {
        std::scoped_lock lock(mutex_);
        const bool hasRoot = root_ != nullptr;
        return WriteValue(serialization, hasRoot) && (!hasRoot || WriteNode(serialization, root_.get()));
    }

    bool Layout::Load(SKSE::SerializationInterface* serialization)
    {
        bool hasRoot = false;
        if (!ReadValue(serialization, hasRoot)) {
            return false;
        }

        bool ok = true;
        auto loaded = hasRoot ? ReadNode(serialization, 0, ok) : nullptr;
        if (!ok) {
            return false;
        }

        std::scoped_lock lock(mutex_);
        root_ = std::move(loaded);
        return true;
    }
}
