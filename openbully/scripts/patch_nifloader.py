# One-shot patch: replace nif_loader.cpp block loop with hierarchy-aware version.
import io, os

path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                    "..", "src", "engine", "formats", "nif_loader.cpp")
path = os.path.normpath(path)
lines = open(path, encoding="utf-8").read().split("\n")

start = next(i for i, l in enumerate(lines) if l.startswith("    // --- Blocks"))
end = next(i for i, l in enumerate(lines)
           if l.strip() == "result.ok = !result.geometries.empty();")

new = """    // --- Blocks ------------------------------------------------------------
    // Each block's declared size lets us skip unknown blocks exactly. Node
    // blocks (NiNode / NiTriShape / NiTriStrips) build the hierarchy table;
    // geometry data blocks are decoded inside their declared range. Everything
    // else is skipped by size.
    std::string lastShapeName;

    result.nodes.reserve(numBlocks);

    for (uint32_t i = 0; i < numBlocks && header.Ok(); i++) {
        const uint32_t blockSize = blockSizes[i];
        if (!header.Need(blockSize)) {
            result.error = "block stream truncated";
            return result;
        }
        const uint8_t* blockStart = header.Pos();
        const uint8_t* blockEnd = blockStart + blockSize;

        const std::string typeName =
            blockTypes[i] < typeNames.size() ? typeNames[blockTypes[i]] : "";

        const bool isShape = (typeName == "NiTriShape" || typeName == "NiTriStrips");
        const bool isNode = isShape || typeName == "NiNode";
        const bool isGeomData = (typeName == "NiTriShapeData" || typeName == "NiTriStripsData");

        if (isNode) {
            Reader block(blockStart, blockSize);
            block.SetBigEndian(result.bigEndian);

            // Every node gets a hierarchy entry in block order, even when the
            // decode fails (children refs stay consistent).
            NifNode node;
            const int32_t nameIndex = block.I32();
            if (nameIndex >= 0 && static_cast<uint32_t>(nameIndex) < strings.size())
                node.name = strings[nameIndex];
            int32_t numExtra = block.I32();
            if (numExtra < 0 || numExtra > 100000) numExtra = 0;
            for (int32_t e = 0; e < numExtra && block.Ok(); e++)
                block.I32();
            block.I32(); // controller

            bool avOk = ReadAVObjectBody(block, &node);

            if (!isShape) {
                // NiNode tail: child refs (i32 count + i32 refs).
                int32_t numChildren = block.I32();
                if (numChildren < 0 || numChildren > 100000) numChildren = 0;
                for (int32_t c = 0; c < numChildren && block.Ok(); c++) {
                    const int32_t ref = block.I32();
                    if (ref >= 0) node.children.push_back(static_cast<uint32_t>(ref));
                }
            } else {
                // NiTriShape/NiTriStrips tail: data ref, skin ref, materials,
                // active material, dirty flag.
                block.I32(); // data reference
                block.I32(); // skin reference
                int32_t numMaterials = block.I32();
                if (numMaterials < 0 || numMaterials > 10000) numMaterials = 0;
                for (int32_t m = 0; m < numMaterials && block.Ok(); m++) {
                    block.I32(); // material name (string index)
                    block.I32(); // material extra data
                }
                block.I32(); // active material
                block.U8();  // "dirty" flag
            }

            if (avOk && block.Ok()) {
                if (isShape) lastShapeName = node.name;
                result.nodes.push_back(std::move(node));
            } else {
                result.nodes.push_back(NifNode{}); // placeholder keeps indices
            }
        } else if (isGeomData) {
            Reader block(blockStart, blockSize);
            block.SetBigEndian(result.bigEndian);

            // NiTriShapeData / NiTriStripsData start directly with the
            // NiGeometryData body - NO NiObjectNET header in 20.3.0.9
            // (verified on cube.nif; see bully-re LireTriShapeData).
            NifGeometryData geo;
            geo.nodeName = lastShapeName;
            geo.nodeIndex = result.nodes.empty()
                ? 0 : static_cast<uint32_t>(result.nodes.size() - 1);
            if (typeName == "NiTriShapeData")
                ReadTriShapeData(block, geo);
            else
                ReadTriStripsData(block, geo);
            if (block.Ok()) {
                result.totalVertices += geo.vertices.size();
                result.totalTriangles += geo.triangles.size();
                result.geometries.push_back(std::move(geo));
            }
        }

        // Advance exactly one declared block, decode success or not.
        header.SeekTo(blockEnd);
    }

"""

lines[start:end] = new.split("\n")
open(path, "w", encoding="utf-8", newline="\n").write("\n".join(lines))
print("replaced lines", start, "to", end)
