/*
 * Copyright (c) Contributors to the Open 3D Engine Project. For complete copyright and license terms please see the LICENSE at the root of this distribution.
 * 
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include <Generation/Components/MeshOptimizer/MeshOptimizerComponent.h>
#include <AzCore/Casting/numeric_cast.h>
#include <AzCore/Debug/Trace.h>
#include <AzCore/RTTI/RTTI.h>
#include <AzCore/Serialization/SerializeContext.h>
#include <AzCore/base.h>
#include <AzCore/std/algorithm.h>
#include <AzCore/std/containers/array.h>
#include <AzCore/std/containers/list.h>
#include <AzCore/std/containers/unordered_map.h>
#include <AzCore/std/containers/vector.h>
#include <AzCore/std/iterator.h>
#include <AzCore/std/limits.h>
#include <AzCore/std/reference_wrapper.h>
#include <AzCore/std/smart_ptr/shared_ptr.h>
#include <AzCore/std/smart_ptr/unique_ptr.h>
#include <AzCore/std/string/string_view.h>
#include <AzCore/std/typetraits/add_pointer.h>
#include <AzCore/std/typetraits/remove_cvref.h>
#include <AzCore/std/utils.h>

#include <SceneAPI/SceneCore/Components/GenerationComponent.h>
#include <SceneAPI/SceneCore/Containers/Scene.h>
#include <SceneAPI/SceneCore/Containers/SceneGraph.h>
#include <SceneAPI/SceneCore/Containers/Views/PairIterator.h>
#include <SceneAPI/SceneCore/Containers/Views/SceneGraphChildIterator.h>
#include <SceneAPI/SceneCore/Containers/Utilities/Filters.h>
#include <SceneAPI/SceneCore/Containers/Views/ConvertIterator.h>
#include <SceneAPI/SceneCore/Containers/Views/FilterIterator.h>
#include <SceneAPI/SceneCore/Containers/Views/View.h>
#include <SceneAPI/SceneCore/DataTypes/GraphData/IBlendShapeData.h>
#include <SceneAPI/SceneCore/DataTypes/GraphData/IMeshData.h>
#include <SceneAPI/SceneCore/DataTypes/GraphData/IMeshVertexBitangentData.h>
#include <SceneAPI/SceneCore/DataTypes/GraphData/IMeshVertexColorData.h>
#include <SceneAPI/SceneCore/DataTypes/GraphData/IMeshVertexTangentData.h>
#include <SceneAPI/SceneCore/DataTypes/GraphData/IMeshVertexUVData.h>
#include <SceneAPI/SceneCore/DataTypes/GraphData/ISkinWeightData.h>
#include <SceneAPI/SceneCore/DataTypes/Groups/IMeshGroup.h>
#include <SceneAPI/SceneCore/DataTypes/ManifestBase/ISceneNodeSelectionList.h>
#include <SceneAPI/SceneCore/DataTypes/Rules/ILodRule.h>
#include <SceneAPI/SceneCore/DataTypes/Rules/ISkinRule.h>
#include <SceneAPI/SceneCore/Events/GenerateEventContext.h>
#include <SceneAPI/SceneCore/Events/ProcessingResult.h>
#include <SceneAPI/SceneCore/Utilities/Reporting.h>
#include <SceneAPI/SceneCore/Utilities/SceneGraphSelector.h>
#include <SceneAPI/SceneData/GraphData/BlendShapeData.h>
#include <SceneAPI/SceneData/GraphData/MeshData.h>
#include <SceneAPI/SceneData/GraphData/MeshVertexBitangentData.h>
#include <SceneAPI/SceneData/GraphData/MeshVertexColorData.h>
#include <SceneAPI/SceneData/GraphData/MeshVertexTangentData.h>
#include <SceneAPI/SceneData/GraphData/MeshVertexUVData.h>
#include <SceneAPI/SceneData/GraphData/SkinWeightData.h>

#include <Generation/Components/MeshOptimizer/MeshBuilder.h>
#include <Generation/Components/MeshOptimizer/MeshBuilderSkinningInfo.h>
#include <Generation/Components/MeshOptimizer/MeshBuilderVertexAttributeLayers.h>

namespace AZ { class ReflectContext; }

namespace AZ::MeshBuilder
{
    using MeshBuilderVertexAttributeLayerColor = MeshBuilderVertexAttributeLayerT<AZ::SceneAPI::DataTypes::Color>;
    AZ_CLASS_ALLOCATOR_IMPL_TEMPLATE(MeshBuilderVertexAttributeLayerColor, AZ::SystemAllocator, 0)
} // namespace AZ::MeshBuilder

namespace AZ::SceneGenerationComponents
{
    using AZ::SceneAPI::Containers::SceneGraph;
    using AZ::SceneAPI::DataTypes::IBlendShapeData;
    using AZ::SceneAPI::DataTypes::ILodRule;
    using AZ::SceneAPI::DataTypes::IMeshData;
    using AZ::SceneAPI::DataTypes::IMeshGroup;
    using AZ::SceneAPI::DataTypes::IMeshVertexBitangentData;
    using AZ::SceneAPI::DataTypes::IMeshVertexTangentData;
    using AZ::SceneAPI::DataTypes::IMeshVertexUVData;
    using AZ::SceneAPI::DataTypes::IMeshVertexColorData;
    using AZ::SceneAPI::DataTypes::ISkinWeightData;
    using AZ::SceneAPI::Events::ProcessingResult;
    using AZ::SceneAPI::Events::GenerateSimplificationEventContext;
    using AZ::SceneAPI::SceneCore::GenerationComponent;
    using AZ::SceneData::GraphData::BlendShapeData;
    using AZ::SceneData::GraphData::MeshData;
    using AZ::SceneData::GraphData::MeshVertexBitangentData;
    using AZ::SceneData::GraphData::MeshVertexColorData;
    using AZ::SceneData::GraphData::MeshVertexTangentData;
    using AZ::SceneData::GraphData::MeshVertexUVData;
    using AZ::SceneData::GraphData::SkinWeightData;
    using NodeIndex = AZ::SceneAPI::Containers::SceneGraph::NodeIndex;
    namespace Containers = AZ::SceneAPI::Containers;
    namespace Views = Containers::Views;

    MeshOptimizerComponent::MeshOptimizerComponent()
    {
        BindToCall(&MeshOptimizerComponent::OptimizeMeshes);
    }

    void MeshOptimizerComponent::Reflect(AZ::ReflectContext* context)
    {
        auto* serializeContext = azrtti_cast<AZ::SerializeContext*>(context);
        if (serializeContext)
        {
            serializeContext->Class<MeshOptimizerComponent, GenerationComponent>()->Version(2);
        }
    }

    template<class MeshDataType, class SkinWeightDataView>
    static AZStd::unique_ptr<AZ::MeshBuilder::MeshBuilderSkinningInfo> ExtractSkinningInfo(
        const MeshDataType* meshData,
        const SkinWeightDataView& skinWeights,
        AZ::u32 maxWeightsPerVertex,
        float weightThreshold)
    {
        if (skinWeights.empty())
        {
            return {};
        }

        const size_t usedControlPointCount = meshData->GetUsedControlPointCount();

        auto skinningInfo = AZStd::make_unique<AZ::MeshBuilder::MeshBuilderSkinningInfo>(aznumeric_cast<AZ::u32>(usedControlPointCount));

        for (const auto& skinData : skinWeights)
        {
            for (size_t controlPointIndex = 0; controlPointIndex < skinData.get().GetVertexCount(); ++controlPointIndex)
            {
                const int usedPointIndex = meshData->GetUsedPointIndexForControlPoint(meshData->GetControlPointIndex(aznumeric_caster(controlPointIndex)));
                const size_t linkCount = skinData.get().GetLinkCount(controlPointIndex);

                if (usedPointIndex < 0 || linkCount == 0)
                {
                    continue;
                }

                for (size_t linkIndex = 0; linkIndex < linkCount; ++linkIndex)
                {
                    const ISkinWeightData::Link& link = skinData.get().GetLink(controlPointIndex, linkIndex);
                    skinningInfo->AddInfluence(usedPointIndex, {aznumeric_caster(link.boneId), link.weight});
                }
            }
        }

        if (skinningInfo)
        {
            skinningInfo->Optimize(maxWeightsPerVertex, weightThreshold);
        }

        return skinningInfo;
    }

    // Recurse through the SceneAPI's iterator types, extracting the real underlying iterator.
    struct ConvertToHierarchyIterator
    {
        template<typename T, typename U>
        static auto Unwrap(const Containers::Views::ConvertIterator<T, U>& it)
        {
            return Unwrap(it.GetBaseIterator());
        }

        template<typename T, typename U>
        static auto Unwrap(const Containers::Views::FilterIterator<T, U>& it)
        {
            return Unwrap(it.GetBaseIterator());
        }

        template<typename T>
        static auto Unwrap(const Containers::Views::SceneGraphChildIterator<T>& it)
        {
            return it.GetHierarchyIterator();
        }
    };

    bool MeshOptimizerComponent::HasAnyBlendShapeChild(const AZ::SceneAPI::Containers::SceneGraph& graph, const AZ::SceneAPI::Containers::SceneGraph::NodeIndex& nodeIndex)
    {
        return !Containers::MakeDerivedFilterView<IBlendShapeData>(
            Views::MakeSceneGraphChildView(graph, nodeIndex, graph.GetContentStorage().cbegin(), true)
        ).empty();
    }

    ProcessingResult MeshOptimizerComponent::OptimizeMeshes(GenerateSimplificationEventContext& context) const
    {
        // Iterate over all graph content and filter out all meshes.
        SceneGraph& graph = context.GetScene().GetGraph();

        // Build a list of mesh data nodes.
        const AZStd::vector<AZStd::pair<const IMeshData*, NodeIndex>> meshes = [](const SceneGraph& graph)
        {
            AZStd::vector<AZStd::pair<const IMeshData*, NodeIndex>> meshes;
            for (auto it = graph.GetContentStorage().cbegin(); it != graph.GetContentStorage().cend(); ++it)
            {
                // Skip anything that isn't a mesh.
                const auto* mesh = azdynamic_cast<const AZ::SceneAPI::DataTypes::IMeshData*>(it->get());
                if (!mesh)
                {
                    continue;
                }

                // Get the mesh data and node index and store them in the vector as a pair, so we can iterate over them later.
                meshes.emplace_back(mesh, graph.ConvertToNodeIndex(it));
            }
            return meshes;
        }(graph);

        const auto meshGroups = Containers::MakeDerivedFilterView<IMeshGroup>(context.GetScene().GetManifest().GetValueStorage());

        const AZStd::unordered_map<const IMeshGroup*, AZStd::vector<AZStd::string_view>> selectedNodes = [&meshGroups]
        {
            AZStd::unordered_map<const IMeshGroup*, AZStd::vector<AZStd::string_view>> selectedNodes;

            const auto addSelectionListToMap = [&selectedNodes](const IMeshGroup& meshGroup, const SceneAPI::DataTypes::ISceneNodeSelectionList& selectionList)
            {
                for (size_t selectedNodeIndex = 0; selectedNodeIndex < selectionList.GetSelectedNodeCount(); ++selectedNodeIndex)
                {
                    selectedNodes[&meshGroup].emplace_back(selectionList.GetSelectedNode(selectedNodeIndex));
                }
            };

            for (const IMeshGroup& meshGroup : meshGroups)
            {
                addSelectionListToMap(meshGroup, meshGroup.GetSceneNodeSelectionList());
                const ILodRule* lodRule = meshGroup.GetRuleContainerConst().FindFirstByType<SceneAPI::DataTypes::ILodRule>().get();
                if (lodRule)
                {
                    for (size_t lod = 0; lod < lodRule->GetLodCount(); ++lod)
                    {
                        addSelectionListToMap(meshGroup, lodRule->GetSceneNodeSelectionList(lod));
                    }
                }
            }
            return selectedNodes;
        }();

        const auto childNodes = [&graph](NodeIndex nodeIndex) { return Views::MakeSceneGraphChildView(graph, nodeIndex, graph.GetContentStorage().cbegin(), true); };
        const auto nodeIndexes = [&graph](const auto& view)
        {
            AZStd::vector<NodeIndex> indexes;
            indexes.reserve(AZStd::distance(view.begin(), view.end()));
            for (auto it = view.begin(); it != view.end(); ++it)
            {
                indexes.emplace_back(graph.ConvertToNodeIndex(ConvertToHierarchyIterator::Unwrap(it)));
            }
            return indexes;
        };

        // Iterate over them. We had to build the array before as this method can insert new nodes, so using the iterator directly would fail.
        for (const auto& [mesh, nodeIndex] : meshes)
        {
            // A Mesh can have multiple child nodes that contain other data streams, like uvs and tangents

            const auto uvDatasView = Containers::MakeDerivedFilterView<IMeshVertexUVData>(childNodes(nodeIndex));
            const auto tangentDatasView = Containers::MakeDerivedFilterView<IMeshVertexTangentData>(childNodes(nodeIndex));
            const auto bitangentDatasView = Containers::MakeDerivedFilterView<IMeshVertexBitangentData>(childNodes(nodeIndex));
            const auto skinWeightDatasView = Containers::MakeDerivedFilterView<ISkinWeightData>(childNodes(nodeIndex));
            const auto colorDatasView = Containers::MakeDerivedFilterView<IMeshVertexColorData>(childNodes(nodeIndex));

            const AZStd::vector<AZStd::reference_wrapper<const IMeshVertexUVData>> uvDatas(uvDatasView.begin(), uvDatasView.end());
            const AZStd::vector<AZStd::reference_wrapper<const IMeshVertexTangentData>> tangentDatas(tangentDatasView.begin(), tangentDatasView.end());
            const AZStd::vector<AZStd::reference_wrapper<const IMeshVertexBitangentData>> bitangentDatas(bitangentDatasView.begin(), bitangentDatasView.end());
            const AZStd::vector<AZStd::reference_wrapper<const ISkinWeightData>> skinWeightDatas(skinWeightDatasView.begin(), skinWeightDatasView.end());
            const AZStd::vector<AZStd::reference_wrapper<const IMeshVertexColorData>> colorDatas(colorDatasView.begin(), colorDatasView.end());

            const AZStd::string_view nodePath(graph.GetNodeName(nodeIndex).GetPath(), graph.GetNodeName(nodeIndex).GetPathLength());

            for (const IMeshGroup& meshGroup : meshGroups)
            {
                // Skip meshes that are not used by this mesh group
                if (AZStd::find(selectedNodes.at(&meshGroup).cbegin(), selectedNodes.at(&meshGroup).cend(), nodePath) == selectedNodes.at(&meshGroup).cend())
                {
                    continue;
                }

                const AZStd::string name =
                    AZStd::string(graph.GetNodeName(nodeIndex).GetName(), graph.GetNodeName(nodeIndex).GetNameLength()).append(SceneAPI::Utilities::OptimizedMeshSuffix);
                if (graph.Find(name).IsValid())
                {
                    AZ_TracePrintf(AZ::SceneAPI::Utilities::LogWindow, "Optimized mesh already exists at '%s', there must be multiple mesh groups that have selected this mesh. Skipping the additional ones.", name.c_str());
                    continue;
                }

                const bool hasBlendShapes = HasAnyBlendShapeChild(graph, nodeIndex);

                auto [optimizedMesh, optimizedUVs, optimizedTangents, optimizedBitangents, optimizedVertexColors, optimizedSkinWeights] = OptimizeMesh(mesh, mesh, uvDatas, tangentDatas, bitangentDatas, colorDatas, skinWeightDatas, meshGroup, hasBlendShapes);

                const NodeIndex optimizedMeshNodeIndex = graph.AddChild(graph.GetNodeParent(nodeIndex), name.c_str(), AZStd::move(optimizedMesh));

                auto addOptimizedNodes = [&graph, &optimizedMeshNodeIndex](const auto& originalNodeIndexes, auto& optimizedNodes)
                {
                    AZ_PUSH_DISABLE_WARNING(, "-Wrange-loop-analysis") // remove when we upgrade from clang 6.0
                    for (const auto& [originalNodeIndex, optimizedNode] : Containers::Views::MakePairView(originalNodeIndexes, optimizedNodes))
                    AZ_POP_DISABLE_WARNING
                    {
                        const AZStd::string optimizedName {graph.GetNodeName(originalNodeIndex).GetName(), graph.GetNodeName(originalNodeIndex).GetNameLength()};
                        const NodeIndex optimizedNodeIndex = graph.AddChild(optimizedMeshNodeIndex, optimizedName.c_str(), AZStd::move(optimizedNode));
                        if (graph.IsNodeEndPoint(originalNodeIndex))
                        {
                            graph.MakeEndPoint(optimizedNodeIndex);
                        }
                    }
                };
                addOptimizedNodes(nodeIndexes(Containers::MakeDerivedFilterView<IMeshVertexUVData>(childNodes(nodeIndex))), optimizedUVs);
                addOptimizedNodes(nodeIndexes(Containers::MakeDerivedFilterView<IMeshVertexTangentData>(childNodes(nodeIndex))), optimizedTangents);
                addOptimizedNodes(nodeIndexes(Containers::MakeDerivedFilterView<IMeshVertexBitangentData>(childNodes(nodeIndex))), optimizedBitangents);
                addOptimizedNodes(nodeIndexes(Containers::MakeDerivedFilterView<IMeshVertexColorData>(childNodes(nodeIndex))), optimizedVertexColors);

                if (optimizedSkinWeights)
                {
                    const NodeIndex optimizedSkinNodeIndex = graph.AddChild(optimizedMeshNodeIndex, "skinWeights", AZStd::move(optimizedSkinWeights));
                    graph.MakeEndPoint(optimizedSkinNodeIndex);
                }

                for (const NodeIndex& blendShapeNodeIndex : nodeIndexes(Containers::MakeDerivedFilterView<IBlendShapeData>(childNodes(nodeIndex))))
                {
                    const IBlendShapeData* blendShapeNode = static_cast<IBlendShapeData*>(graph.GetNodeContent(blendShapeNodeIndex).get());
                    auto [optimizedBlendShape, _1, _2, _3 , _4, _5] = OptimizeMesh(blendShapeNode, mesh, {}, {}, {}, {}, {}, meshGroup, hasBlendShapes);

                    const AZStd::string optimizedName {graph.GetNodeName(blendShapeNodeIndex).GetName(), graph.GetNodeName(blendShapeNodeIndex).GetNameLength()};
                    const NodeIndex optimizedNodeIndex = graph.AddChild(optimizedMeshNodeIndex, optimizedName.c_str(), AZStd::move(optimizedBlendShape));
                    if (graph.IsNodeEndPoint(blendShapeNodeIndex))
                    {
                        graph.MakeEndPoint(optimizedNodeIndex);
                    }
                }

                const AZStd::array optimizedChildTypes {
                    azrtti_typeid<IMeshData>(),
                    azrtti_typeid<IMeshVertexUVData>(),
                    azrtti_typeid<IMeshVertexTangentData>(),
                    azrtti_typeid<IMeshVertexBitangentData>(),
                    azrtti_typeid<IMeshVertexColorData>(),
                    azrtti_typeid<ISkinWeightData>(),
                    azrtti_typeid<IBlendShapeData>(),
                };
                for (const NodeIndex& childNodeIndex : nodeIndexes(childNodes(nodeIndex)))
                {
                    const AZStd::shared_ptr<SceneAPI::DataTypes::IGraphObject>& childNode = graph.GetNodeContent(childNodeIndex);

                    if (!AZStd::any_of(optimizedChildTypes.begin(), optimizedChildTypes.end(), [&childNode](const AZ::Uuid& typeId) { return AZ::RttiIsTypeOf(typeId, childNode.get()); }))
                    {
                        const AZStd::string optimizedName {graph.GetNodeName(childNodeIndex).GetName(), graph.GetNodeName(childNodeIndex).GetNameLength()};
                        const NodeIndex optimizedNodeIndex = graph.AddChild(optimizedMeshNodeIndex, optimizedName.c_str(), childNode);
                        if (graph.IsNodeEndPoint(childNodeIndex))
                        {
                            graph.MakeEndPoint(optimizedNodeIndex);
                        }
                    }
                }
            }
        }

        return ProcessingResult::Success;
    }

    template<class DataNodeType, class MeshBuilderLayerType>
    AZStd::vector<AZStd::unique_ptr<DataNodeType>> makeSceneGraphNodesForMeshBuilderLayers(const MeshBuilderLayerType& meshBuilderLayers)
    {
        AZStd::vector<AZStd::unique_ptr<DataNodeType>> layers(meshBuilderLayers.size());
        AZStd::generate(layers.begin(), layers.end(), []
        {
            return AZStd::make_unique<DataNodeType>();
        });
        return layers;
    };


    template<class MeshDataType>
    AZStd::tuple<
        AZStd::unique_ptr<MeshDataType>,
        AZStd::vector<AZStd::unique_ptr<MeshVertexUVData>>,
        AZStd::vector<AZStd::unique_ptr<MeshVertexTangentData>>,
        AZStd::vector<AZStd::unique_ptr<MeshVertexBitangentData>>,
        AZStd::vector<AZStd::unique_ptr<MeshVertexColorData>>,
        AZStd::unique_ptr<AZ::SceneAPI::DataTypes::ISkinWeightData>
    > MeshOptimizerComponent::OptimizeMesh(
        const MeshDataType* meshData,
        const IMeshData* baseMesh,
        const AZStd::vector<AZStd::reference_wrapper<const IMeshVertexUVData>>& uvs,
        const AZStd::vector<AZStd::reference_wrapper<const IMeshVertexTangentData>>& tangents,
        const AZStd::vector<AZStd::reference_wrapper<const IMeshVertexBitangentData>>& bitangents,
        const AZStd::vector<AZStd::reference_wrapper<const IMeshVertexColorData>>& vertexColors,
        const AZStd::vector<AZStd::reference_wrapper<const ISkinWeightData>>& skinWeights,
        const AZ::SceneAPI::DataTypes::IMeshGroup& meshGroup,
        bool hasBlendShapes)
    {
        const size_t vertexCount = meshData->GetUsedControlPointCount();

        AZ::MeshBuilder::MeshBuilder meshBuilder(vertexCount, AZStd::numeric_limits<size_t>::max(), AZStd::numeric_limits<size_t>::max(), /*optimizeDuplicates=*/ !hasBlendShapes);

        // Make the layers to hold the vertex data
        auto* orgVtxLayer = meshBuilder.AddLayer<MeshBuilder::MeshBuilderVertexAttributeLayerUInt32>(vertexCount);
        auto* posLayer = meshBuilder.AddLayer<MeshBuilder::MeshBuilderVertexAttributeLayerVector3>(vertexCount, false, true);
        auto* normalsLayer = meshBuilder.AddLayer<MeshBuilder::MeshBuilderVertexAttributeLayerVector3>(vertexCount, false, true);

        const auto makeLayersForData = [&meshBuilder, vertexCount](const auto& dataView)
        {
            using InputDataType = typename AZStd::remove_cvref_t<decltype(*dataView.begin())>::type;

            // Determine the layer data type to use in the mesh builder based on the type of scene graph node
            // IMeshVertexUVData -> MeshBuilderVertexAttributeLayerVector2
            // IMeshVertexTangentData -> MeshBuilderVertexAttributeLayerVector4
            // IMeshVertexBitangentData -> MeshBuilderVertexAttributeLayerVector3
            // IMeshVertexColorData -> MeshBuilderVertexAttributeLayerColor
            struct ViewTypeToLayerType
            {
                static constexpr auto type(const IMeshVertexUVData*) -> MeshBuilder::MeshBuilderVertexAttributeLayerVector2;
                static constexpr auto type(const IMeshVertexTangentData*) -> MeshBuilder::MeshBuilderVertexAttributeLayerVector4;
                static constexpr auto type(const IMeshVertexBitangentData*) -> MeshBuilder::MeshBuilderVertexAttributeLayerVector3;
                static constexpr auto type(const IMeshVertexColorData*) -> MeshBuilder::MeshBuilderVertexAttributeLayerColor;
            };
            using ResultingLayerType = decltype(ViewTypeToLayerType::type(AZStd::add_pointer_t<InputDataType>{}));

            // the views provided by SceneAPI do not have a size() method, so compute it
            const size_t layerCount = AZStd::distance(dataView.begin(), dataView.end());
            AZStd::vector<ResultingLayerType*> layers(layerCount);
            AZStd::generate(layers.begin(), layers.end(), [&meshBuilder, vertexCount]
            {
                return meshBuilder.AddLayer<ResultingLayerType>(vertexCount);
            });
            return layers;
        };
        const AZStd::vector<MeshBuilder::MeshBuilderVertexAttributeLayerVector2*> uvLayers = makeLayersForData(uvs);
        const AZStd::vector<MeshBuilder::MeshBuilderVertexAttributeLayerVector4*> tangentLayers = makeLayersForData(tangents);
        const AZStd::vector<MeshBuilder::MeshBuilderVertexAttributeLayerVector3*> bitangentLayers = makeLayersForData(bitangents);
        const AZStd::vector<MeshBuilder::MeshBuilderVertexAttributeLayerColor*> vertexColorLayers = makeLayersForData(vertexColors);

        const auto* skinRule = meshGroup.GetRuleContainerConst().FindFirstByType<SceneAPI::DataTypes::ISkinRule>().get();
        const AZ::u32 maxWeightsPerVertex = skinRule ? skinRule->GetMaxWeightsPerVertex() : 4;
        const float weightThreshold = skinRule ? skinRule->GetWeightThreshold() : 0.001f;
        meshBuilder.SetSkinningInfo(ExtractSkinningInfo(meshData, skinWeights, maxWeightsPerVertex, weightThreshold));

        // Add the vertex data to all the layers
        const AZ::u32 faceCount = meshData->GetFaceCount();
        for (AZ::u32 faceIndex = 0; faceIndex < faceCount; ++faceIndex)
        {
            meshBuilder.BeginPolygon(baseMesh->GetFaceMaterialId(faceIndex));
            for (const AZ::u32 vertexIndex : meshData->GetFaceInfo(faceIndex).vertexIndex)
            {
                const int orgVertexNumber = meshData->GetUsedPointIndexForControlPoint(meshData->GetControlPointIndex(vertexIndex));
                AZ_Assert(orgVertexNumber >= 0, "Invalid vertex number");
                orgVtxLayer->SetCurrentVertexValue(orgVertexNumber);

                posLayer->SetCurrentVertexValue(meshData->GetPosition(vertexIndex));
                normalsLayer->SetCurrentVertexValue(meshData->GetNormal(vertexIndex));

                AZ_PUSH_DISABLE_WARNING(, "-Wrange-loop-analysis") // remove when we upgrade from clang 6.0
                for (const auto& [uvData, uvLayer] : Containers::Views::MakePairView(uvs, uvLayers))
                {
                    uvLayer->SetCurrentVertexValue(uvData.get().GetUV(vertexIndex));
                }
                for (const auto& [tangentData, tangentLayer] : Containers::Views::MakePairView(tangents, tangentLayers))
                {
                    tangentLayer->SetCurrentVertexValue(tangentData.get().GetTangent(vertexIndex));
                }
                for (const auto& [bitangentData, bitangentLayer] : Containers::Views::MakePairView(bitangents, bitangentLayers))
                {
                    bitangentLayer->SetCurrentVertexValue(bitangentData.get().GetBitangent(vertexIndex));
                }
                for (const auto& [vertexColorData, vertexColorLayer] : Containers::Views::MakePairView(vertexColors, vertexColorLayers))
                {
                    vertexColorLayer->SetCurrentVertexValue(vertexColorData.get().GetColor(vertexIndex));
                }
                AZ_POP_DISABLE_WARNING

                meshBuilder.AddPolygonVertex(orgVertexNumber);
            }

            meshBuilder.EndPolygon();
        }
        meshBuilder.GenerateSubMeshVertexOrders();

        // Create the resulting nodes
        struct ResultingType
        {
            // When this method is called with an IMeshData node, it is generating a MeshData node. When called on an
            // IBlendShapeData node, it is generating a BlendShapeData node.
            static constexpr auto type(const IMeshData*) -> MeshData;
            static constexpr auto type(const IBlendShapeData*) -> BlendShapeData;
        };

        auto optimizedMesh = AZStd::make_unique<decltype(ResultingType::type(meshData))>();
        optimizedMesh->CloneAttributesFrom(meshData);

        AZStd::vector<AZStd::unique_ptr<MeshVertexUVData>> optimizedUVs = makeSceneGraphNodesForMeshBuilderLayers<MeshVertexUVData>(uvLayers);
        AZStd::vector<AZStd::unique_ptr<MeshVertexTangentData>> optimizedTangents = makeSceneGraphNodesForMeshBuilderLayers<MeshVertexTangentData>(tangentLayers);
        AZStd::vector<AZStd::unique_ptr<MeshVertexBitangentData>> optimizedBitangents = makeSceneGraphNodesForMeshBuilderLayers<MeshVertexBitangentData>(bitangentLayers);
        AZStd::vector<AZStd::unique_ptr<MeshVertexColorData>> optimizedVertexColors = makeSceneGraphNodesForMeshBuilderLayers<MeshVertexColorData>(vertexColorLayers);

        // Copy node attributes
        AZStd::apply([](const auto&&... nodePairView) {
            ((AZStd::for_each(begin(nodePairView), end(nodePairView), [](const auto& nodePair) {
                auto& originalNode = nodePair.first;
                auto& optimizedNode = nodePair.second;
                optimizedNode->CloneAttributesFrom(&originalNode.get());
            })), ...);
        }, std::tuple {
            Views::MakePairView(uvs, optimizedUVs),
            Views::MakePairView(tangents, optimizedTangents),
            Views::MakePairView(bitangents, optimizedBitangents),
            Views::MakePairView(vertexColors, optimizedVertexColors),
        });

        unsigned int indexOffset = 0;
        for (size_t subMeshIndex = 0; subMeshIndex < meshBuilder.GetNumSubMeshes(); ++subMeshIndex)
        {
            const AZ::MeshBuilder::MeshBuilderSubMesh* subMesh = meshBuilder.GetSubMesh(subMeshIndex);
            for (size_t vertexIndex = 0; vertexIndex < subMesh->GetNumVertices(); ++vertexIndex)
            {
                const AZ::MeshBuilder::MeshBuilderVertexLookup& vertexLookup = subMesh->GetVertex(vertexIndex);
                optimizedMesh->AddPosition(posLayer->GetVertexValue(vertexLookup.mOrgVtx, vertexLookup.mDuplicateNr));
                optimizedMesh->AddNormal(normalsLayer->GetVertexValue(vertexLookup.mOrgVtx, vertexLookup.mDuplicateNr));
                optimizedMesh->SetVertexIndexToControlPointIndexMap(
                    aznumeric_caster(optimizedMesh->GetVertexCount() - 1),
                    orgVtxLayer->GetVertexValue(vertexLookup.mOrgVtx, vertexLookup.mDuplicateNr)
                );

                for (auto [uvLayer, optimizedUVNode] : Containers::Views::MakePairView(uvLayers, optimizedUVs))
                {
                    optimizedUVNode->AppendUV(uvLayer->GetVertexValue(vertexLookup.mOrgVtx, vertexLookup.mDuplicateNr));
                }
                for (auto [tangentLayer, optimizedTangentNode] : Containers::Views::MakePairView(tangentLayers, optimizedTangents))
                {
                    optimizedTangentNode->AppendTangent(tangentLayer->GetVertexValue(vertexLookup.mOrgVtx, vertexLookup.mDuplicateNr));
                }
                for (auto [bitangentLayer, optimizedBitangentNode] : Containers::Views::MakePairView(bitangentLayers, optimizedBitangents))
                {
                    optimizedBitangentNode->AppendBitangent(bitangentLayer->GetVertexValue(vertexLookup.mOrgVtx, vertexLookup.mDuplicateNr));
                }
                for (auto [vertexColorLayer, optimizedVertexColorNode] : Containers::Views::MakePairView(vertexColorLayers, optimizedVertexColors))
                {
                    optimizedVertexColorNode->AppendColor(vertexColorLayer->GetVertexValue(vertexLookup.mOrgVtx, vertexLookup.mDuplicateNr));
                }
            }
            AZStd::unordered_set<size_t> usedIndexes;
            for (size_t polygonIndex = 0; polygonIndex < subMesh->GetNumPolygons(); ++polygonIndex)
            {
                AddFace(
                    optimizedMesh.get(),
                    aznumeric_caster(indexOffset + subMesh->GetIndex(polygonIndex * 3 + 0)),
                    aznumeric_caster(indexOffset + subMesh->GetIndex(polygonIndex * 3 + 1)),
                    aznumeric_caster(indexOffset + subMesh->GetIndex(polygonIndex * 3 + 2)),
                    aznumeric_caster(subMesh->GetMaterialIndex())
                );
                const auto& faceInfo = optimizedMesh->GetFaceInfo(optimizedMesh->GetFaceCount() - 1);
                AZStd::copy(AZStd::begin(faceInfo.vertexIndex), AZStd::end(faceInfo.vertexIndex), AZStd::inserter(usedIndexes, usedIndexes.begin()));
            }
            indexOffset += usedIndexes.size();
        }

        AZStd::unique_ptr<SkinWeightData> optimizedSkinWeights;
        if (MeshBuilder::MeshBuilderSkinningInfo* skinningInfo = meshBuilder.GetSkinningInfo())
        {
            optimizedSkinWeights = AZStd::make_unique<SkinWeightData>();

            const size_t skinnedVertexCount = skinningInfo->GetNumOrgVertices();
            optimizedSkinWeights->ResizeContainerSpace(skinnedVertexCount);

            for (size_t vertex = 0; vertex < skinnedVertexCount; ++vertex)
            {
                const size_t boneCountAffectingThisVertex = skinningInfo->GetNumInfluences(vertex);
                for (size_t influencingBone = 0; influencingBone < boneCountAffectingThisVertex; ++influencingBone)
                {
                    const MeshBuilder::MeshBuilderSkinningInfo::Influence& influence = skinningInfo->GetInfluence(vertex, influencingBone);
                    const int boneId = optimizedSkinWeights->GetBoneId(skinWeights[0].get().GetBoneName(aznumeric_caster(influence.mNodeNr)));
                    optimizedSkinWeights->AppendLink(vertex, {boneId, influence.mWeight});
                }
            }
        }

        return AZStd::make_tuple(
            AZStd::move(optimizedMesh),
            AZStd::move(optimizedUVs),
            AZStd::move(optimizedTangents),
            AZStd::move(optimizedBitangents),
            AZStd::move(optimizedVertexColors),
            AZStd::move(optimizedSkinWeights)
        );
    }

    void MeshOptimizerComponent::AddFace(AZ::SceneData::GraphData::BlendShapeData* blendShape, unsigned int index1, unsigned int index2, unsigned int index3, [[maybe_unused]] unsigned int faceMaterialId)
    {
        blendShape->AddFace({index1, index2, index3});
    }
    void MeshOptimizerComponent::AddFace(AZ::SceneData::GraphData::MeshData* mesh, unsigned int index1, unsigned int index2, unsigned int index3, unsigned int faceMaterialId)
    {
        mesh->AddFace({index1, index2, index3}, faceMaterialId);
    }
} // namespace AZ::SceneGenerationComponents
