import unreal

ASSET_PATH = "/Game/Materials"
ASSET_NAME = "M_PaletteGlow"
PACKAGE_PATH = f"{ASSET_PATH}/{ASSET_NAME}"

mel = unreal.MaterialEditingLibrary
eal = unreal.EditorAssetLibrary
asset_tools = unreal.AssetToolsHelpers.get_asset_tools()

# ---------- helpers ----------
def try_set(obj, names, value):
    for n in names:
        try:
            obj.set_editor_property(n, value)
            return True
        except Exception:
            pass
    return False

def list_exprs(mat):
    try:
        return mel.get_material_expressions(mat)
    except Exception:
        return []

def find_node(mat, cls, desc=None):
    for x in list_exprs(mat):
        if isinstance(x, cls):
            if desc and hasattr(x, "description") and x.description == desc:
                return x
            if not desc:
                return x
    return None

def find_param_by_name(mat, cls, pname):
    pname_l = str(pname).lower()
    for x in list_exprs(mat):
        if isinstance(x, cls):
            for prop in ("parameter_name", "ParameterName"):
                try:
                    if str(getattr(x, prop)).lower() == pname_l:
                        return x
                except Exception:
                    pass
    return None

def create(material, cls, x, y):
    return mel.create_material_expression(material, cls, x, y)

def connect(src, dst, input_name, out_name=""):
    try:
        mel.connect_material_expressions(src, out_name, dst, input_name)
    except Exception as ex:
        print(f"[Warn] Failed to connect {src} -> {dst}.{input_name}: {ex}")

def connect_try_names(src, dst, names):
    for nm in names:
        try:
            mel.connect_material_expressions(src, "", dst, nm)
            return True
        except Exception:
            continue
    print(f"[Warn] Could not connect {src} to any of {names}")
    return False

# ---------- create/load material ----------
if eal.does_asset_exist(PACKAGE_PATH):
    material = eal.load_asset(PACKAGE_PATH)
    print(f"[Info] Reusing material: {PACKAGE_PATH}")
else:
    factory = unreal.MaterialFactoryNew()
    material = asset_tools.create_asset(ASSET_NAME, ASSET_PATH, unreal.Material, factory)
    print(f"[Info] Created material: {PACKAGE_PATH}")

# ---------- material props ----------
try_set(material, ["blend_mode", "BlendMode"], unreal.BlendMode.BLEND_OPAQUE)
try_set(material, ["shading_model","ShadingModel","shading_models","ShadingModels"], unreal.MaterialShadingModel.MSM_UNLIT)
try_set(material, ["two_sided","TwoSided"], False)

# ---------- nodes ----------
# TexCoord
texcoord = find_node(material, unreal.MaterialExpressionTextureCoordinate)
if not texcoord:
    texcoord = create(material, unreal.MaterialExpressionTextureCoordinate, -1400, -120)

# Time
time_node = find_node(material, unreal.MaterialExpressionTime)
if not time_node:
    time_node = create(material, unreal.MaterialExpressionTime, -1400, 80)

# Speed param
speed = find_param_by_name(material, unreal.MaterialExpressionScalarParameter, "Speed")
if not speed:
    speed = create(material, unreal.MaterialExpressionScalarParameter, -1100, 360)
    try_set(speed, ["parameter_name","ParameterName"], "Speed")
    try_set(speed, ["default_value","DefaultValue"], 70.0)

# Texture object param
tex_param = find_param_by_name(material, unreal.MaterialExpressionTextureObjectParameter, "BasePaletteTex")
if not tex_param:
    tex_param = create(material, unreal.MaterialExpressionTextureObjectParameter, -1100, -40)
    try_set(tex_param, ["parameter_name","ParameterName"], "BasePaletteTex")

# TextureSample (will sample BasePaletteTex at UV from CustomUV)
tex_samp = find_node(material, unreal.MaterialExpressionTextureSample)
if not tex_samp:
    tex_samp = create(material, unreal.MaterialExpressionTextureSample, -360, -40)
# Prefer COLOR sampler; harmless if engine overrides based on actual texture
try_set(tex_samp, ["sampler_type","SamplerType"], unreal.MaterialSamplerType.SAMPLERTYPE_COLOR)

# Custom UV (FLOAT2)
cust_uv = find_node(material, unreal.MaterialExpressionCustom, "Palette Cell UV")
if not cust_uv:
    cust_uv = create(material, unreal.MaterialExpressionCustom, -700, -60)
try_set(cust_uv, ["description","Description"], "Palette Cell UV")
try_set(cust_uv, ["output_type","OutputType"], unreal.CustomMaterialOutputType.CMOT_FLOAT2)

# Inputs for Custom UV: [UV]
uv_inputs = []
ci = unreal.CustomInput()
if not try_set(ci, ["InputName","input_name"], "UV"):
    raise RuntimeError("CustomInput.InputName not found.")
uv_inputs.append(ci)
cust_uv.set_editor_property("inputs", uv_inputs)

cust_uv_code = r"""
float2 cell = floor(saturate(UV) * 16.0);
int col = (int)cell.x;
int row = (int)cell.y;
return (float2(col + 0.5, row + 0.5)) / 16.0;
"""
try_set(cust_uv, ["code","Code"], cust_uv_code)

# Custom Main (FLOAT3)
cust_main = find_node(material, unreal.MaterialExpressionCustom, "Palette Glow Update")
if not cust_main:
    cust_main = create(material, unreal.MaterialExpressionCustom, -80, 20)
try_set(cust_main, ["description","Description"], "Palette Glow Update")
try_set(cust_main, ["output_type","OutputType"], unreal.CustomMaterialOutputType.CMOT_FLOAT3)

# Inputs for Custom Main: [UV, TimeSeconds, Speed, BaseSample]
main_inputs = []
for nm in ["UV", "TimeSeconds", "Speed", "BaseSample"]:
    ci = unreal.CustomInput()
    if not try_set(ci, ["InputName","input_name"], nm):
        raise RuntimeError("CustomInput.InputName not found.")
    main_inputs.append(ci)
cust_main.set_editor_property("inputs", main_inputs)

cust_main_code = r"""
float2 cell = floor(saturate(UV) * 16.0);
int col = (int)cell.x;
int row = (int)cell.y;
int index = row * 16 + col;

float tickF    = TimeSeconds * Speed;
int   tick     = (int)floor(tickF);
float tickFrac = frac(tickF);

// If outside 226..254, use sampled base color passed in
if (index < 226 || index > 254)
{
    return BaseSample;
}

const float inv63 = 1.0 / 63.0;

// Glow waves
float3 glowSpeed = float3(1, 2, 3);
float3 phase   = (tick * glowSpeed) / (2.0 * 43.0);
float3 glowSaw = abs(frac(phase) * 2.0 - 1.0);
float3 glows   = 20.0 + glowSaw * 43.0;

// 226..233: cycling reds
if (index >= 226 && index <= 233)
{
    int offset = index - 226;
    int cyc = (((tick + offset) % 8) + 1) * 6 + 23;
    cyc = min(cyc, 63);
    return float3(cyc * inv63, 0, 0);
}

int   localBase = 100000 + tick * 97;
float n1 = 32 + floor(frac(sin(localBase +  1) * 43758.5453123) * 32.0);
float n2 = 32 + floor(frac(sin(localBase +  2) * 43758.5453123) * 32.0);
float n3 = 32 + floor(frac(sin(localBase +  3) * 43758.5453123) * 32.0);
float n4 = 32 + floor(frac(sin(localBase +  4) * 43758.5453123) * 32.0);
float n5 = 32 + floor(frac(sin(localBase +  5) * 43758.5453123) * 32.0);
float n7 = 32 + floor(frac(sin(localBase +  7) * 43758.5453123) * 32.0);
float n8 = 32 + floor(frac(sin(localBase +  8) * 43758.5453123) * 32.0);

float pulse10 = step(frac(sin(localBase + 17) * 43758.5453123), 0.02) * exp(-saturate(tickFrac) * 4.0) * 63.0;
float pulse11 = step(frac(sin(localBase + 18) * 43758.5453123), 0.02) * exp(-saturate(tickFrac) * 6.0) * 63.0;
float pulse12 = step(frac(sin(localBase + 19) * 43758.5453123), 0.02) * exp(-saturate(tickFrac) * 4.0) * 63.0;

float3 c = 0;
switch (index)
{
    case 234: c = float3(n1, 0, 0); break;
    case 235: c = float3(n2, 0, 0); break;
    case 236: c = float3(n3, 0, 0); break;
    case 237: c = float3(n4, n4 * 0.5, 0); break;
    case 238: c = float3(n5, n5 * 0.5, 0); break;
    case 239: c = float3(n7, max(n7 - 10, 0), 0); break;
    case 240: c = float3(n8, max(n8 - 10, 0), 0); break;
    case 241: c = float3(n1, n1, n1); break;
    case 242: c = float3(0, n4, 0); break;
    case 243: c = float3(0, 0, n5); break;
    case 244: c = float3(pulse10, pulse10, 0); break;
    case 245: c = float3(pulse11, pulse11, pulse11); break;
    case 246: c = float3(pulse12, pulse12, 0); break;
    case 247: c = float3(glows.x, glows.y, glows.z); break;
    case 248: c = float3(glows.z, glows.z, glows.z); break;
    case 249: c = float3(glows.x, 0, glows.x); break;
    case 250: c = float3(glows.x, 0, 0); break;
    case 251: c = float3(glows.z, 0, glows.x); break;
    case 252: c = float3(0, glows.y, 0); break;
    case 253: c = float3(0, glows.y, glows.z); break;
    case 254: c = float3(0, 0, glows.z); break;
    default:  c = 0; break;
}

return c * inv63;
"""
try_set(cust_main, ["code","Code"], cust_main_code)

# ---------- wiring ----------
# CustomUV <- TexCoord
connect(texcoord, cust_uv, "Inputs[0]")  # index-based works for Custom

# TextureSample UVs <- CustomUV
connect_try_names(cust_uv, tex_samp, ["Coordinates", "UVs"])

# TextureSample Texture <- BasePaletteTex (param)
# Different engine versions expose this pin name differently; try all.
connect_try_names(tex_param, tex_samp, ["TextureObject", "Texture", "Tex"])

# CustomMain numeric inputs (order: UV, TimeSeconds, Speed, BaseSample)
connect(texcoord,    cust_main, "Inputs[0]")   # UV
connect(time_node,   cust_main, "Inputs[1]")   # TimeSeconds
connect(speed,       cust_main, "Inputs[2]")   # Speed
connect(tex_samp,    cust_main, "Inputs[3]")   # BaseSample (RGB)

# Emissive <- CustomMain
try:
    mel.connect_material_property(cust_main, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
except Exception as ex:
    print(f"[Warn] Could not connect emissive: {ex}")

# ---------- tidy, compile, save ----------
try:
    mel.layout_material_expressions(material)
except Exception:
    pass

try:
    mel.recompile_material(material)
except Exception as ex:
    print(f"[Warn] Recompile failed: {ex}")

eal.save_asset(PACKAGE_PATH, only_if_is_dirty=False)
print(f"[Success] Material updated: {PACKAGE_PATH}")

