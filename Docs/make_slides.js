// Builds Docs/UsdOptixViewer-code.pptx, a walkthrough of the UsdOptixViewer code.
//   cd Docs && npm install pptxgenjs@3.12.0 && node make_slides.js
// The theme colors are applied afterwards by the pptx skill's apply_theme.js
// when APPLY_THEME points to it (otherwise PowerPoint's stock palette is used).
const pptxgen = require("pptxgenjs");
const path = require("path");

const THEME = {
    name: "VizTools Graphite",
    headFontFace: "Cambria",
    bodyFontFace: "Calibri",
    colors: {
        dk1: "1B1F24", lt1: "FFFFFF", dk2: "2D343C", lt2: "EEF2EE",
        accent1: "76B900", accent2: "E8A33D", accent3: "3E8E7E", accent4: "8B95A1",
        accent5: "C9584B", accent6: "5B7DB1", hlink: "3E8E7E", folHlink: "8B95A1",
    },
};
const HEX = THEME.colors;
const MONO = "Courier New";

const pres = new pptxgen();
pres.layout = "LAYOUT_16x9";  // 10 x 5.625 in
pres.title = "UsdOptixViewer: how the code works";
pres.author = "VizTools";
pres.theme = { headFontFace: THEME.headFontFace, bodyFontFace: THEME.bodyFontFace };
const C = pres.SchemeColor;
const FOOTER = "UsdOptixViewer · code walkthrough";

// ---- layouts ------------------------------------------------------------------
pres.defineSlideMaster({
    title: "TITLE",
    background: { color: C.text1 },
    objects: [
        { placeholder: { options: { name: "title", type: "title", x: 0.6, y: 1.55, w: 8.8, h: 1.1, fontSize: 44,
                                    bold: true, color: C.background1, valign: "bottom", align: "left", margin: 0 },
                         text: "Title" } },
        { placeholder: { options: { name: "body", type: "body", x: 0.6, y: 2.8, w: 8.8, h: 1.2, fontSize: 18,
                                    color: C.accent1, valign: "top", margin: 0 },
                         text: "Subtitle" } },
    ],
});
pres.defineSlideMaster({
    title: "TITLE_IMAGE",
    background: { color: C.text1 },
    objects: [
        { placeholder: { options: { name: "title", type: "title", x: 0.6, y: 1.55, w: 4.4, h: 1.1, fontSize: 34,
                                    bold: true, color: C.background1, valign: "bottom", align: "left", margin: 0 },
                         text: "Title" } },
        { placeholder: { options: { name: "body", type: "body", x: 0.6, y: 2.8, w: 4.4, h: 1.4, fontSize: 18,
                                    color: C.accent1, valign: "top", align: "left", margin: 0 },
                         text: "Subtitle" } },
    ],
});
pres.defineSlideMaster({
    title: "SECTION",
    background: { color: C.text2 },
    objects: [
        { placeholder: { options: { name: "title", type: "title", x: 1.9, y: 1.9, w: 7.5, h: 0.9, fontSize: 36,
                                    bold: true, color: C.background1, valign: "middle", align: "left", margin: 0 },
                         text: "Section" } },
        { placeholder: { options: { name: "body", type: "body", x: 1.9, y: 2.85, w: 7.5, h: 0.9, fontSize: 16,
                                    color: C.background2, valign: "top", margin: 0 },
                         text: "What this section covers" } },
    ],
    slideNumber: { x: 9.0, y: 5.2, w: 0.6, h: 0.3, fontSize: 10, color: C.accent4, align: "right" },
});
pres.defineSlideMaster({
    title: "CONTENT",
    background: { color: C.background1 },
    margin: [0.5, 0.5, 0.6, 0.5],
    objects: [
        { placeholder: { options: { name: "title", type: "title", x: 0.5, y: 0.3, w: 9.0, h: 0.75, fontSize: 30,
                                    bold: true, color: C.text1, valign: "middle", align: "left", margin: 0 },
                         text: "Title" } },
        { text: { text: FOOTER, options: { x: 0.5, y: 5.2, w: 6, h: 0.3, fontSize: 10, color: C.accent4, margin: 0 } } },
    ],
    slideNumber: { x: 9.0, y: 5.2, w: 0.5, h: 0.3, fontSize: 10, color: C.accent4, align: "right" },
});

// ---- building blocks ------------------------------------------------------------
let objectCount = 0;
const IMAGES = path.join(__dirname, "images");

// A screenshot of the running viewer; w or h may be omitted and follows the image's aspect.
function shot(slide, file, x, y, w, h, aspect, alt) {
    if (!h) h = w / aspect;
    if (!w) w = h * aspect;
    slide.addImage({ path: path.join(IMAGES, file), x, y, w, h, altText: alt, objectName: id("shot") });
    return { w, h };
}

function caption(slide, x, y, w, head, body) {
    slide.addText([{ text: head, options: { bold: true, fontSize: 14, breakLine: true } },
                   { text: body, options: { fontSize: 12, color: C.text2 } }],
                  { x, y, w, h: 0.75, valign: "top", color: C.text1, margin: 0, isTextBox: true,
                    objectName: id("caption") });
}
const id = (kind) => `${kind}-${++objectCount}`;

// The deck's motif: a green circle with a number or letter.
function badge(slide, x, y, label, size = 0.42, fill = C.accent1, textColor = C.text1) {
    slide.addShape(pres.shapes.OVAL, { x, y, w: size, h: size, fill: { color: fill }, line: { color: fill },
                                       objectName: id("badge") });
    slide.addText(label, { x, y, w: size, h: size, fontSize: size > 0.5 ? 20 : 14, bold: true, color: textColor,
                           align: "center", valign: "middle", margin: 0, isTextBox: true, objectName: id("badge-text") });
}

function card(slide, x, y, w, h, opts) {
    slide.addShape(pres.shapes.ROUNDED_RECTANGLE, {
        x, y, w, h, rectRadius: 0.08, fill: { color: opts.fill || C.background2 }, line: { color: opts.fill || C.background2 },
        objectName: id("card"),
    });
    let top = y + 0.15;
    if (opts.badge) badge(slide, x + 0.15, y + 0.15, opts.badge);
    const textX = opts.badge ? x + 0.68 : x + 0.2;
    if (opts.head) {
        slide.addText(opts.head, { x: textX, y: top, w: x + w - textX - 0.15, h: 0.42, fontSize: 16, bold: true,
                                   color: opts.headColor || C.text1, valign: "middle", margin: 0, isTextBox: true,
                                   objectName: id("card-head") });
        top += 0.5;
    }
    if (opts.body) {
        const runs = Array.isArray(opts.body)
            ? opts.body.map((t, i) => ({ text: t, options: { bullet: opts.bullets !== false, breakLine: i < opts.body.length - 1,
                                                              paraSpaceAfter: 4 } }))
            : opts.body;
        slide.addText(runs, { x: x + 0.2, y: top, w: w - 0.35, h: y + h - top - 0.1, fontSize: opts.size || 14,
                              color: opts.bodyColor || C.text1, valign: "top", margin: 0, isTextBox: true,
                              objectName: id("card-body") });
    }
}

function code(slide, x, y, w, h, lines, size = 11) {
    slide.addShape(pres.shapes.ROUNDED_RECTANGLE, { x, y, w, h, rectRadius: 0.06, fill: { color: C.text1 },
                                                    line: { color: C.text1 }, objectName: id("code-bg") });
    slide.addText(lines.map((l, i) => ({ text: l, options: { breakLine: i < lines.length - 1 } })), {
        x: x + 0.2, y: y + 0.12, w: w - 0.4, h: h - 0.24, fontFace: MONO, fontSize: size, color: C.background1,
        valign: "top", margin: 0, isTextBox: true, objectName: id("code"),
    });
}

function box(slide, x, y, w, h, label, sub, fill, textColor) {
    slide.addShape(pres.shapes.ROUNDED_RECTANGLE, { x, y, w, h, rectRadius: 0.08, fill: { color: fill }, line: { color: fill },
                                                    objectName: id("box") });
    slide.addText([{ text: label, options: { bold: true, fontSize: 14, breakLine: !!sub } },
                   ...(sub ? [{ text: sub, options: { fontSize: 11 } }] : [])],
                  { x: x + 0.08, y, w: w - 0.16, h, align: "center", valign: "middle", color: textColor, margin: 0,
                    isTextBox: true, objectName: id("box-text") });
}

function arrow(slide, x1, y1, x2, y2, color = HEX.accent4) {
    slide.addShape(pres.shapes.LINE, {
        x: Math.min(x1, x2), y: Math.min(y1, y2), w: Math.abs(x2 - x1), h: Math.abs(y2 - y1),
        flipH: x2 < x1, flipV: y2 < y1, line: { color, width: 2, endArrowType: "triangle" }, objectName: id("arrow"),
    });
}

function content(title, section) {
    const s = pres.addSlide({ masterName: "CONTENT", sectionTitle: section });
    s.addText(title, { placeholder: "title" });
    return s;
}

function divider(number, title, sub, section) {
    pres.addSection({ title: section });
    const s = pres.addSlide({ masterName: "SECTION", sectionTitle: section });
    badge(s, 0.6, 1.95, String(number), 0.8);
    s.addText(title, { placeholder: "title" });
    s.addText(sub, { placeholder: "body" });
    return s;
}

// ---- 1. title -------------------------------------------------------------------
pres.addSection({ title: "Overview" });
{
    const s = pres.addSlide({ masterName: "TITLE_IMAGE", sectionTitle: "Overview" });
    shot(s, "title-materials.jpg", 5.3, 0, null, 5.625, 520 / 622,
         "Ray-traced spheres in gold and red plastic, from rough to mirror-smooth, on a checkered floor");
    s.addText("UsdOptixViewer", { placeholder: "title" });
    s.addText("How the code works: from a USD stage to ray-traced pixels, and on to a headset",
              { placeholder: "body" });
    s.addText("VizTools · ginebra · October 2026", { x: 0.6, y: 4.7, w: 4.4, h: 0.4, fontSize: 12, color: C.accent4,
                                                    margin: 0, isTextBox: true, objectName: "title-date" });
    s.addNotes("A walkthrough of the UsdOptixViewer source code, in the order the data flows: loading the USD " +
               "stage, ray tracing it with OptiX, the desktop application, and the XR output of the cloudxr branch.");
}

// ---- 2. at a glance -------------------------------------------------------------
{
    const s = content("The program at a glance", "Overview");
    const stats = [["17", "source files", "C++, CUDA and CMake"], ["4,400", "lines of code", "plus stb_image"],
                   ["5", "libraries", "OpenUSD, EnTT, OptiX, SDL3 + ImGui, OpenXR"], ["143", "book pages", "every line explained"]];
    stats.forEach(([n, label, sub], i) => {
        const x = 0.5 + i * 2.3;
        s.addText(n, { x, y: 1.2, w: 2.1, h: 0.85, fontSize: 48, bold: true, color: C.accent1, margin: 0,
                       fontFace: THEME.headFontFace, isTextBox: true, objectName: id("stat") });
        s.addText([{ text: label, options: { bold: true, fontSize: 16, breakLine: true } },
                   { text: sub, options: { fontSize: 12, color: C.accent4 } }],
                  { x, y: 2.05, w: 2.1, h: 0.8, valign: "top", color: C.text1, margin: 0, isTextBox: true,
                    objectName: id("stat-label") });
    });
    card(s, 0.5, 3.0, 9.0, 2.0, {
        head: "What it does", bullets: true, size: 14, body: [
            "Opens .usd/.usda/.usdc/.usdz stages, shows the prim tree and every property",
            "Ray traces the geometry with UsdPreviewSurface materials, textures, shadows and glossy reflections",
            "Looks through USD cameras; on the cloudxr branch, streams stereo to a headset through CloudXR",
        ] });
    s.addNotes("Counts are for the cloudxr branch. main has 13 files: it lacks XrOutput and CloudXrService. " +
               "The literate book in LiterateP/ contains every line of the code with prose.");
}

{
    const s = content("The viewer at work", "Overview");
    const { w } = shot(s, "viewer-ui.jpg", 0.5, 1.25, null, 3.35, 1854 / 985,
                       "The viewer window: prim tree on the left, ray-traced textured cube in the middle, inspector on the right");
    const x = 0.5 + w + 0.3;
    [["Hierarchy", "prims of the stage, filter by name"], ["Viewport", "OptiX image; click to pick"],
     ["Inspector", "prim, material, properties"], ["Status bar", "counts, camera, samples"]].forEach(([h, b], i) => {
        badge(s, x, 1.25 + i * 0.95, String(i + 1));
        caption(s, x + 0.52, 1.25 + i * 0.95, 9.5 - x - 0.52, h, b);
    });
    s.addNotes("textured_scene.usda with --select /World/Box: the selected cube is highlighted, revealed in the " +
               "tree and described in the inspector, with its texture, roughness and every USD attribute.");
}

// ---- 3. architecture ------------------------------------------------------------
{
    const s = content("Architecture: data flows left to right", "Overview");
    const y = 1.5, h = 1.1, w = 1.95;
    box(s, 0.5, y, w, h, "USD stage", "OpenUSD", HEX.accent2, C.text1);
    box(s, 2.85, y, w, h, "Scene", "EnTT registry + FlatGeometry", C.text2, C.background1);
    box(s, 5.2, y, w, h, "OptixRenderer", "GAS, SBT, launch", C.text2, C.background1);
    box(s, 7.55, y, w, h, "Device programs", "devicePrograms.cu, on the GPU", C.accent1, C.text1);
    arrow(s, 2.45, y + h / 2, 2.85, y + h / 2);
    arrow(s, 4.8, y + h / 2, 5.2, y + h / 2);
    arrow(s, 7.15, y + h / 2, 7.55, y + h / 2);
    box(s, 2.85, 3.35, w, 1.0, "App", "SDL3 window, ImGui panels", C.background2, C.text1);
    box(s, 7.55, 3.35, w, 1.0, "XrOutput", "OpenXR / CloudXR", C.background2, C.text1);
    arrow(s, 3.825, 3.35, 3.825, 2.6);     // App drives the scene
    arrow(s, 5.2, 3.85, 4.8, 3.85);        // pixels back to the app
    arrow(s, 8.525, 2.6, 8.525, 3.35);     // eyes to the headset
    s.addText("pixels + mesh ids", { x: 5.2, y: 3.45, w: 2.0, h: 0.3, fontSize: 11, color: C.accent4, margin: 0,
                                     isTextBox: true, objectName: "label-pixels" });
    s.addText([{ text: "LaunchParams.h", options: { bold: true } },
               { text: " is the one structure both CPU and GPU read" }],
              { x: 0.5, y: 4.6, w: 9.0, h: 0.4, fontSize: 14, color: C.text1, margin: 0, isTextBox: true,
                objectName: "label-launch" });
    s.addNotes("Scene.* converts USD into an EnTT registry and flat world-space triangle arrays. OptixRenderer.* " +
               "uploads them and launches the OptiX programs in devicePrograms.cu. App.* owns the window, the " +
               "panels and the camera. XrOutput (cloudxr branch) sends eye images to an OpenXR runtime.");
}

// ---- section 1: scene -----------------------------------------------------------------
divider(1, "Loading the scene", "Scene.h / Scene.cpp: USD stage → EnTT registry → world-space triangles", "Scene");

{
    const s = content("One entity per prim", "Scene");
    const comps = [["PrimInfo", "path, name, type name"], ["Hierarchy", "parent and children entities"],
                   ["MeshGeometry", "triangle range, material, mesh id"], ["CameraView", "index into Scene::cameras()"]];
    comps.forEach(([name, what], i) => {
        const y = 1.3 + i * 0.88;
        s.addShape(pres.shapes.ROUNDED_RECTANGLE, { x: 0.5, y, w: 4.3, h: 0.72, rectRadius: 0.08,
                                                    fill: { color: C.background2 }, line: { color: C.background2 },
                                                    objectName: id("comp") });
        badge(s, 0.65, y + 0.15, String.fromCharCode(65 + i));
        s.addText([{ text: name, options: { bold: true, fontFace: MONO, breakLine: true } },
                   { text: what, options: { fontSize: 12, color: C.text2 } }],
                  { x: 1.2, y, w: 3.5, h: 0.72, fontSize: 14, valign: "middle", margin: 0, color: C.text1,
                    isTextBox: true, objectName: id("comp-text") });
    });
    card(s, 5.2, 1.3, 4.3, 3.52, {
        head: "Why a registry", bullets: true, body: [
            "The tree panel walks Hierarchy; the inspector reads the prim back by PrimInfo::path",
            "Only drawn prims get MeshGeometry, so \"is it rendered?\" is one lookup",
            "Picking maps a mesh id back to its entity",
            "Instance proxies are included, so instanced models appear in full",
        ] });
    s.addNotes("createEntities walks the stage with UsdTraverseInstanceProxies and creates one entity per prim. " +
               "Components are added by later passes: extractGeometry adds MeshGeometry, extractCameras adds CameraView.");
}

{
    const s = content("Flattening the geometry", "Scene");
    const steps = [["Triangulate", "Mesh faces fan-split; Cube and Sphere generated"],
                   ["Orient", "leftHanded meshes rewound, so e1 × e2 is the outward normal"],
                   ["Shade", "authored normals, smooth for subdivision, flat for \"none\""],
                   ["Transform", "UsdGeomXformCache to world space, bounds grown"]];
    steps.forEach(([head, body], i) => {
        const x = 0.5 + i * 2.3;
        badge(s, x, 1.35, String(i + 1));
        s.addText(head, { x: x + 0.52, y: 1.35, w: 1.6, h: 0.42, fontSize: 16, bold: true, color: C.text1,
                          valign: "middle", margin: 0, isTextBox: true, objectName: id("step-head") });
        s.addText(body, { x, y: 1.9, w: 2.1, h: 1.0, fontSize: 13, color: C.text1, valign: "top", margin: 0,
                          isTextBox: true, objectName: id("step-body") });
        if (i < 3) arrow(s, x + 2.05, 1.56, x + 2.3, 1.56);
    });
    code(s, 0.5, 3.1, 9.0, 1.85, [
        "struct FlatGeometry {                  // one soup, ready for one GAS",
        "    vertices, indices;                 // shared positions",
        "    triMesh, triMaterial;              // per triangle",
        "    triUVs (6), triNormals (9);        // per triangle corner",
        "    materials, textures, meshEntity, bounds;",
        "};",
    ], 12);
    s.addNotes("Everything the GPU needs per hit is indexed by the triangle index OptiX reports, so all the " +
               "attribute arrays are per triangle. A zero shading normal means flat shading.");
}

{
    const s = content("Materials and textures", "Scene");
    card(s, 0.5, 1.3, 4.35, 3.6, {
        head: "UsdPreviewSurface subset", badge: "M", body: [
            "diffuseColor: constant or texture × scale + bias",
            "normal: tangent-space map, frame from positions and st",
            "roughness, metallic, ior: constants or any texture channel",
            "useSpecularWorkflow with specularColor",
            "opacity + opacityThreshold: alpha cutouts",
            "GeomSubsets give faces their own material",
        ], size: 13 });
    card(s, 5.15, 1.3, 4.35, 3.6, {
        head: "Textures", badge: "T", body: [
            "Read through ArResolver, so files inside .usdz work",
            "Decoded with stb_image, cached by path and settings",
            "CPU-built mip chain, averaged in linear space for sRGB",
            "Uploaded as mipmapped CUDA arrays with trilinear, 8× anisotropic filtering",
            "Unsupported inputs become inspector warnings",
        ], size: 13 });
    s.addNotes("materialFor follows UsdShade connections through node graphs to the UsdPreviewSurface inputs. " +
               "scalarInput handles packed maps: one texture channel per input, each with its own scale and bias.");
}

{
    const s = content("What the renderer draws", "Scene");
    const cells = [["materials.jpg", "Roughness and metallic", "gold and plastic chart, GGX reflections",
                    "Spheres from mirror-smooth to rough in gold and red plastic, reflected in the floor"],
                   ["textures.jpg", "Textures", "UV grids; every \"UP\" arrow points up",
                    "A cube and a wall with a checker and arrow texture"],
                   ["normal-maps.jpg", "Normal maps", "every bump lit on its upper right",
                    "Three walls with bump normal maps above a textured floor"],
                   ["mipmaps.jpg", "Mipmaps", "a far floor without moiré",
                    "A large floor with a repeated texture, filtered smoothly in the distance"]];
    cells.forEach(([file, head, body, alt], i) => {
        const x = 0.5 + (i % 2) * 4.6, y = 1.2 + Math.floor(i / 2) * 1.95;
        shot(s, file, x, y, 2.55, 1.7, 1.5, alt);
        caption(s, x + 2.75, y + 0.05, 1.75, head, body);
    });
    s.addNotes("The four material test scenes in UsdOptixViewer/assets, each after 256 accumulated samples.");
}

// ---- section 2: OptiX -------------------------------------------------------------------
divider(2, "Ray tracing with OptiX", "OptixRenderer.* on the host, devicePrograms.cu on the GPU", "OptiX");

{
    const s = content("The renderer on the host", "OptiX");
    const steps = ["CUDA primary context", "OptiX device context", "Module from embedded PTX",
                   "Program groups → pipeline", "Shader binding table"];
    steps.forEach((t, i) => {
        const y = 1.3 + i * 0.68;
        badge(s, 0.5, y + 0.06, String(i + 1));
        s.addText(t, { x: 1.05, y, w: 3.4, h: 0.55, fontSize: 15, color: C.text1, valign: "middle", margin: 0,
                       isTextBox: true, objectName: id("init-step") });
    });
    card(s, 4.8, 1.3, 4.7, 1.45, {
        head: "setGeometry(FlatGeometry)", body: [
            "Uploads the arrays and textures",
            "Builds one compacted GAS over all triangles",
        ] });
    card(s, 4.8, 2.95, 4.7, 2.05, {
        head: "render()", body: [
            "Uploads LaunchParams, calls optixLaunch",
            "Copies colors back; mesh ids too on frame 0",
            "frameIndex counts samples; resetAccumulation() sets it to 0",
        ] });
    s.addNotes("init runs once (or again on another GPU in the cloudxr branch, through release()). The PTX is " +
               "compiled by nvcc at build time and embedded with cmake/EmbedFile.cmake, so the binary is self-contained.");
}

{
    const s = content("LaunchParams: the CPU–GPU contract", "OptiX");
    code(s, 0.5, 1.3, 5.3, 3.6, [
        "struct LaunchParams {",
        "    uchar4* colorBuffer;   // average",
        "    float4* accumBuffer;   // running sums",
        "    unsigned* meshIdBuffer;// for picking",
        "    unsigned width, height, frameIndex;",
        "    Camera camera;         // eye, u, v, w,",
        "                           // ortho, clipping",
        "    float3 lightDir;",
        "    unsigned shadows, mipmaps, ...;",
        "    OptixTraversableHandle handle;",
        "    const float3* vertices; ...",
        "};",
    ], 12);
    card(s, 6.1, 1.3, 3.4, 3.6, {
        head: "Rules", body: [
            "One header, compiled by g++ and nvcc",
            "Change both sides together",
            "The camera has no trigonometry on the GPU: ray = w + x·u + y·v",
            "Orthographic: shift the origin instead",
        ] });
    s.addNotes("Camera::w is unit length for desktop cameras. XR eyes use an off-centre w for asymmetric frusta, " +
               "which is why they use no clipping range.");
}

{
    const s = content("The device programs", "OptiX");
    const progs = [["raygen", "Jittered sample per pixel; accumulates; writes mesh id on frame 0", C.accent1],
                   ["closest-hit", "Lambert + GGX, shadow ray, one VNDF glossy bounce, firefly clamp", C.accent2],
                   ["any-hit", "Alpha cutout: ignore hits below opacityThreshold", C.accent3],
                   ["miss ×2", "Radiance: gradient sky. Shadow: light is visible", C.accent4]];
    progs.forEach(([name, what, color], i) => {
        const x = 0.5 + (i % 2) * 4.6, y = 1.3 + Math.floor(i / 2) * 1.6;
        s.addShape(pres.shapes.ROUNDED_RECTANGLE, { x, y, w: 4.4, h: 1.4, rectRadius: 0.08,
                                                    fill: { color: C.background2 }, line: { color: C.background2 },
                                                    objectName: id("prog") });
        badge(s, x + 0.15, y + 0.15, String(i + 1), 0.42, color);
        s.addText(name, { x: x + 0.68, y: y + 0.15, w: 3.5, h: 0.42, fontFace: MONO, fontSize: 16, bold: true,
                          color: C.text1, valign: "middle", margin: 0, isTextBox: true, objectName: id("prog-name") });
        s.addText(what, { x: x + 0.2, y: y + 0.65, w: 4.0, h: 0.7, fontSize: 13, color: C.text1, valign: "top",
                          margin: 0, isTextBox: true, objectName: id("prog-text") });
    });
    s.addText("Payload: rgb, mesh id, depth, rng seed · max trace depth 3 (camera → reflection → shadow)",
              { x: 0.5, y: 4.55, w: 9.0, h: 0.4, fontSize: 13, color: C.text2, margin: 0, isTextBox: true,
                objectName: "payload" });
    s.addNotes("sampleTexture picks the mip level from ray differentials: rays through neighbouring pixels are " +
               "intersected with the triangle's plane and turned into st gradients for tex2DGrad.");
}

{
    const s = content("Progressive rendering", "OptiX");
    const n = [1, 2, 4, 8, 16, 32, 64, 128, 256];
    s.addChart(pres.charts.LINE, [{ name: "Relative noise", labels: n.map(String), values: n.map((k) => 1 / Math.sqrt(k)) }], {
        x: 0.5, y: 1.25, w: 5.4, h: 3.7, showTitle: true, title: "Noise falls as 1/√N", titleFontSize: 14,
        titleFontFace: "+mn-lt", titleColor: HEX.dk1, chartColors: [HEX.accent1], lineSize: 3, lineDataSymbol: "circle",
        lineDataSymbolSize: 7, showLegend: false, catAxisTitle: "samples per pixel", showCatAxisTitle: true,
        catAxisTitleFontSize: 11, catAxisTitleColor: HEX.accent4, catAxisLabelColor: HEX.accent4,
        valAxisLabelColor: HEX.accent4, catAxisLabelFontSize: 11, valAxisLabelFontSize: 11,
        catAxisLabelFontFace: "+mn-lt", valAxisLabelFontFace: "+mn-lt", catAxisTitleFontFace: "+mn-lt",
        valAxisLabelFormatCode: "0.00", valGridLine: { color: "DDE2DD", size: 0.5 }, catGridLine: { style: "none" },
    });
    card(s, 6.2, 1.3, 3.3, 3.6, {
        head: "How the app drives it", body: [
            "Any change sets m_needsRender",
            "updateCamera() + resetAccumulation()",
            "One sample per frame until 256",
            "Frame 0 hits pixel centres, so picking is exact",
        ] });
    s.addNotes("The accumulation buffer holds float sums; the displayed color is the average with an " +
               "approximate sRGB (square root) encoding. Antialiasing comes for free from the jitter.");
}

// ---- section 3: application ---------------------------------------------------------------
divider(3, "The application", "App.h / App.cpp: SDL3, Dear ImGui, cameras and picking", "Application");

{
    const s = content("One turn of the main loop", "Application");
    const steps = [["Events", "SDL events, drops"], ["Open", "pending file, loaded off the dialog thread"],
                   ["XR", "headset frame (cloudxr branch)"], ["UI", "menus and four panels"], ["Present", "SDL_Renderer, vsync"]];
    steps.forEach(([h, b], i) => {
        const x = 0.5 + i * 1.86;
        box(s, x, 1.35, 1.66, 1.2, h, b, i === 2 ? HEX.accent1 : C.background2, C.text1);
        if (i < 4) arrow(s, x + 1.66, 1.95, x + 1.86, 1.95);
    });
    card(s, 0.5, 2.9, 4.4, 2.05, {
        head: "Panels", body: ["Hierarchy: tree with name filter", "Inspector: prim, camera, material, properties",
                               "Viewport: image, mouse, picking", "Status bar: counts, camera, samples, XR"] });
    card(s, 5.1, 2.9, 4.4, 2.05, {
        head: "Immediate mode", body: ["No widget objects: drawUi() describes the whole UI each frame",
                                       "State lives in plain App members", "Tests drive the UI from command-line flags, not the mouse"] });
    s.addNotes("Synthetic X input does not reach SDL on this machine, which is why --select and --camera exist: " +
               "they let scripted screenshots test selection and cameras.");
}

{
    const s = content("Two kinds of camera", "Application");
    card(s, 0.5, 1.3, 4.35, 2.75, {
        head: "Orbit camera", badge: "O", body: [
            "Target, distance, yaw, pitch",
            "Follows the stage's up axis (Y or Z)",
            "Left drag orbits, right/middle pans, wheel zooms",
            "Framing: d = r / sin(φ/2) + 15%",
        ] });
    card(s, 5.15, 1.3, 4.35, 2.75, {
        head: "USD cameras", badge: "U", body: [
            "GfCamera → GfFrustum at the start time",
            "Perspective or orthographic, clipping range as depth along w",
            "Film back fitted inside the viewport (usdview's \"fit\")",
            "Moving the view hands off to the orbit camera without a jump",
        ] });
    s.addText([{ text: "Test scene: ", options: { bold: true } },
               { text: "assets/camera_test.usda has five cameras: Front, Telephoto, orthographic Top, Clipped and a scaled Rig/Side" }],
              { x: 0.5, y: 4.3, w: 9.0, h: 0.6, fontSize: 14, color: C.text1, margin: 0, isTextBox: true,
                objectName: "camera-test" });
    s.addNotes("Camera menu, the inspector's Look through button, or --camera. orbitFromUsdCamera inverts the " +
               "orbit formulas so the first drag continues from the USD camera's pose. assets/camera_test.usda " +
               "has five test cameras.");
}

{
    const s = content("Looking through USD cameras", "Application");
    [["camera-front.jpg", "Front", "perspective, 35 mm lens", "Front view: cube left, sphere, pyramid right"],
     ["camera-top.jpg", "Top", "orthographic, 14 × 14 units", "Straight-down orthographic view of the ground and objects"],
     ["camera-clipped.jpg", "Clipped", "near plane at 10.4 cuts the scene", "The front view with the near half clipped away"]]
        .forEach(([file, head, body, alt], i) => {
            const x = 0.5 + i * 3.1;
            shot(s, file, x, 1.2, 2.8, 2.8, 1, alt);
            caption(s, x, 4.1, 2.8, head, body);
        });
    s.addNotes("assets/camera_test.usda, started with --camera /World/Cameras/<name>. The window is fitted inside " +
               "the square viewport, so the 3:2 film back shows extra scene above and below.");
}

// ---- section 4: XR ---------------------------------------------------------------------------
divider(4, "XR output", "cloudxr branch: XrOutput.* and CloudXrService.*", "XR");

{
    const s = content("From OptiX to the headset", "XR");
    const y = 1.45, h = 1.0;
    box(s, 0.5, y, 1.6, h, "OptiX", "renderView()", C.accent1, C.text1);
    box(s, 2.4, y, 1.75, h, "Shared buffer", "Vulkan memory, CUDA view", C.text2, C.background1);
    box(s, 4.45, y, 1.6, h, "Swapchain", "copy on Vulkan queue", C.text2, C.background1);
    box(s, 6.35, y, 1.5, h, "CloudXR", "OpenXR runtime", HEX.accent2, C.text1);
    box(s, 8.15, y, 1.35, h, "Headset", "Vision Pro, Quest", C.background2, C.text1);
    [[2.1, 2.4], [4.15, 4.45], [6.05, 6.35], [7.85, 8.15]].forEach(([a, b]) => arrow(s, a, y + h / 2, b, y + h / 2));
    const stats = [["2 × 2400×1920", "eye images (Vision Pro profile)"], ["2.8 ms", "per frame, both eyes, 1 sample"],
                   ["0", "copies through main memory"]];
    stats.forEach(([n, label], i) => {
        const x = 0.5 + i * 3.1;
        s.addText(n, { x, y: 2.85, w: 2.9, h: 0.7, fontSize: 30, bold: true, color: C.accent1, margin: 0,
                       fontFace: THEME.headFontFace, isTextBox: true, objectName: id("xr-stat") });
        s.addText(label, { x, y: 3.55, w: 2.9, h: 0.5, fontSize: 13, color: C.text1, margin: 0, isTextBox: true,
                           objectName: id("xr-label") });
    });
    s.addText("Memory is exported from Vulkan as an opaque fd and imported with cudaImportExternalMemory, " +
              "so CUDA and the runtime must share one GPU (matched by UUID).",
              { x: 0.5, y: 4.25, w: 9.0, h: 0.7, fontSize: 13, color: C.text2, margin: 0, isTextBox: true,
                objectName: "xr-note" });
    s.addNotes("The CloudXR runtime accepts only Vulkan apps (XR_KHR_vulkan_enable2). The runtime creates the " +
               "Vulkan instance and device; Vulkan itself is dlopened, so no Vulkan SDK is needed to build.");
}

{
    const s = content("The headset view on the desktop", "XR");
    shot(s, "xr-mirror.jpg", 0.5, 1.2, null, 3.8, 925 / 740,
         "The left eye of the headset: the test scene seen from standing height, with a very wide field of view");
    card(s, 5.5, 1.2, 4.0, 3.8, {
        head: "--xr-mirror", body: [
            "The left eye, copied back from the shared buffer",
            "CloudXR 6.2.1 in-process, Vision Pro profile, no headset attached",
            "Head placed at the Front camera's eye, (0, 3, 10)",
            "4 samples per pixel: 4.8 ms per frame for both eyes",
        ] });
    s.addNotes("The eyes have a field of view of about 112 × 100 degrees, which is why the scene looks small in " +
               "the mirror: a headset shows the same image across most of the user's view.");
}

{
    const s = content("The XR session, step by step", "XR");
    const states = ["Off", "Waiting", "SystemReady", "Idle", "Running"];
    states.forEach((t, i) => {
        const x = 0.5 + i * 1.86;
        box(s, x, 1.3, 1.66, 0.75, t, null, i === 4 ? HEX.accent1 : C.background2, C.text1);
        if (i < 4) arrow(s, x + 1.66, 1.675, x + 1.86, 1.675);
    });
    card(s, 0.5, 2.35, 4.4, 2.6, {
        head: "What happens", body: [
            "Waiting: xrGetSystem retried; auto profiles wait for a client",
            "SystemReady: renderer moves to the runtime's GPU",
            "Running: xrWaitFrame paces the loop; vsync off",
        ] });
    card(s, 5.1, 2.35, 4.4, 2.6, {
        head: "Where the user stands", body: [
            "XR origin at the desktop camera's eye, facing its horizontal direction",
            "1 m of head motion = 1/metersPerUnit units × world scale",
            "Asymmetric eye frusta: w aims at the window centre",
        ] });
    s.addNotes("--cloudxr starts the CloudXR service in-process (CloudXrService dlopens libcloudxr.so and " +
               "restores SDL's signal handlers); --xr uses a runtime already running, e.g. from Isaac Teleop. " +
               "CLOUDXR.md lists every difference from main.");
}

// ---- wrap-up ----------------------------------------------------------------------------------
pres.addSection({ title: "Wrap-up" });
{
    const s = content("Build, test and document", "Wrap-up");
    card(s, 0.5, 1.3, 2.85, 2.2, {
        head: "Build", badge: "B", body: ["CMake 3.28, C++20", "PTX embedded", "OpenXR fetched", "No warnings"] });
    card(s, 3.575, 1.3, 2.85, 2.2, {
        head: "Test", badge: "T", body: ["Six test scenes", "xwd screenshots", "usdchecker", "CloudXR, no headset"] });
    card(s, 6.65, 1.3, 2.85, 2.2, {
        head: "Document", badge: "D", body: ["LiterateP/ book", "lp.py check", "logbook.md", "CLOUDXR.md"] });
    code(s, 0.5, 3.75, 9.0, 1.2, [
        "cmake -S UsdOptixViewer -B build && cmake --build build -j",
        "./build/UsdOptixViewer assets/textured_scene.usda --select /World/Box",
        "python3 LiterateP/lp.py check      # must print ok for every file",
    ], 12);
    s.addNotes("CLAUDE.md has the exact build, test and book commands. Every code change must also update the " +
               "matching chunk in LiterateP/src and pass lp.py check.");
}

{
    const s = pres.addSlide({ masterName: "TITLE", sectionTitle: "Wrap-up" });
    s.addText("Where to start reading", { placeholder: "title" });
    s.addText([
        { text: "LaunchParams.h", options: { bold: true, color: C.background1 } },
        { text: "  →  ", options: { color: C.accent4 } },
        { text: "Scene.cpp", options: { bold: true, color: C.background1 } },
        { text: "  →  ", options: { color: C.accent4 } },
        { text: "devicePrograms.cu", options: { bold: true, color: C.background1 } },
        { text: "  →  ", options: { color: C.accent4 } },
        { text: "App.cpp", options: { bold: true, color: C.background1 } },
    ], { placeholder: "body", fontSize: 16 });
    s.addText("Or read LiterateP/UsdOptixViewer-literate.pdf from the start: same code, with the reasons.",
              { x: 0.6, y: 4.3, w: 8.8, h: 0.5, fontSize: 14, color: C.accent1, margin: 0, isTextBox: true,
                objectName: "close-note" });
    s.addNotes("The shared header shows what the GPU needs; Scene.cpp shows how USD becomes that; the device " +
               "programs show how it becomes light; App.cpp ties it together.");
}

(async () => {
    const out = path.join(__dirname, "UsdOptixViewer-code.pptx");
    await pres.writeFile({ fileName: out });
    if (process.env.APPLY_THEME) {
        const { applyTheme } = require(process.env.APPLY_THEME);
        await applyTheme(out, THEME);
    }
    console.log("wrote " + out);
})();
