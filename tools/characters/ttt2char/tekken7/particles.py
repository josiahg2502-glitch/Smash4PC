"""Tekken 7's particle systems (Unreal Engine 4.14 Cascade, cooked): the emitters of a ParticleSystem, their first
LOD's modules and the values of their distributions.

A ParticleSystem export lists its Emitters (ParticleSpriteEmitter); an emitter's LODLevels[0] (ParticleLODLevel)
names its RequiredModule, SpawnModule, TypeDataModule (mesh, GPU sprites, ...) and its other Modules, each an export
of the same package whose class names the module (ParticleModuleSize, ...). A module's distributions are cooked into
lookup tables: {Op, EntryCount, EntryStride, SubEntryStride, TimeScale, TimeBias, Values}; Op 1 = one value per
entry, 2 = a random range (each entry the minimum then the maximum), 3 = an extreme range; entry i holds the value at
time TimeBias + i / TimeScale (one entry: a constant). A distribution that was not cooked into a table names a
Distribution* object (its Constant, Min/Max or ConstantCurve).

Written for this project from the files themselves (no third-party code).
"""
import re

from .uasset import export_properties, load, read_object
from .material import Materials, short_ref


def _path(ref):
    """"Class'/Game/a/b.b:Emitter.Sub'" -> '/Game/a/b.b:Emitter.Sub' (export names repeat under different outers:
    every emitter has its own ParticleLODLevel_0, so objects are told apart by their whole path)."""
    if not isinstance(ref, str):
        return ref
    m = re.match(r"^[^']*'(.*)'$", ref)
    return m.group(1) if m else ref


def _vector(v, dim):
    if isinstance(v, dict):
        if all(k in v for k in 'xyz'):
            return [float(v['x']), float(v['y']), float(v['z'])][:dim]
        if all(k in v for k in 'rgb'):
            return [float(v['r']), float(v['g']), float(v['b'])][:dim]
    if isinstance(v, (list, tuple)):
        return [float(x) for x in v][:dim]
    return [float(v)] * dim


class Distribution:
    """A float (dim 1) or vector (dim 3) distribution: `samples` = [(time, minimum, maximum)] (lists of dim)."""

    def __init__(self, samples, dim):
        self.dim = dim
        self.samples = samples or [(0.0, [0.0] * dim, [0.0] * dim)]

    @classmethod
    def constant(cls, value, dim=1):
        v = _vector(value, dim)
        return cls([(0.0, v, list(v))], dim)

    @classmethod
    def from_table(cls, t, dim):
        op, n = t.get('Op', 1), max(1, t.get('EntryCount', 1) or 1)
        stride = t.get('EntryStride') or dim
        sub = t.get('SubEntryStride') or 0
        vals = [float(x) for x in t.get('Values', [])]
        if not vals:
            return None
        dim = max(1, min(dim, sub or stride))
        scale, bias = float(t.get('TimeScale') or 0.0), float(t.get('TimeBias') or 0.0)
        samples = []
        for i in range(n):
            e = vals[i * stride:(i + 1) * stride]
            if len(e) < dim:
                break
            lo = e[:dim]
            hi = e[sub:sub + dim] if op in (2, 3) and sub and len(e) >= sub + dim else lo
            samples.append((bias + (i / scale if scale else 0.0), lo, hi))
        return cls(samples, dim)

    @property
    def is_constant(self):
        return len(self.samples) == 1

    def at(self, t):
        """(minimum, maximum) at time t (linear between the samples, held outside them)."""
        s = self.samples
        if t <= s[0][0] or len(s) == 1:
            return s[0][1], s[0][2]
        for (t0, lo0, hi0), (t1, lo1, hi1) in zip(s, s[1:]):
            if t <= t1:
                a = (t - t0) / (t1 - t0) if t1 > t0 else 1.0
                return ([x + (y - x) * a for x, y in zip(lo0, lo1)], [x + (y - x) * a for x, y in zip(hi0, hi1)])
        return s[-1][1], s[-1][2]

    def mean(self, t=0.0):
        lo, hi = self.at(t)
        return [(a + b) / 2 for a, b in zip(lo, hi)]

    def spread(self, t=0.0):
        """Half the random range at time t."""
        lo, hi = self.at(t)
        return [abs(b - a) / 2 for a, b in zip(lo, hi)]

    def peak(self):
        return max(max(max(lo), max(hi)) for _, lo, hi in self.samples)


class Module:
    def __init__(self, cls, props, objects):
        self.cls = cls.replace('ParticleModule', '') or cls
        self.props = props
        self._objects = objects

    def get(self, name, default=None):
        return self.props.get(name, default)

    def dist(self, name, dim=1, default=None):
        """A distribution property (`name`) -> Distribution, or `default` (a value) when the module has none."""
        v = self.props.get(name)
        if isinstance(v, dict):
            if isinstance(v.get('Table'), dict):
                d = Distribution.from_table(v['Table'], dim)
                if d is not None:
                    return d
            ref = v.get('Distribution')
            if ref:
                d = self._object_distribution(_path(ref), dim)
                if d is not None:
                    return d
        return Distribution.constant(default, dim) if default is not None else None

    def _object_distribution(self, name, dim):
        cls, p = self._objects.get(name, (None, None))
        if not cls or p is None:
            return None
        if 'Constant' in cls and 'Curve' not in cls:
            return Distribution.constant(p.get('Constant', 0.0), dim)
        if 'Uniform' in cls:
            lo, hi = _vector(p.get('Min', 0.0), dim), _vector(p.get('Max', 0.0), dim)
            return Distribution([(0.0, lo, hi)], dim)
        if 'ConstantCurve' in cls:
            pts = (p.get('ConstantCurve') or {}).get('Points') or []
            out = [(float(q.get('InVal', 0.0)), _vector(q.get('OutVal', 0.0), dim), _vector(q.get('OutVal', 0.0), dim))
                   for q in pts]
            return Distribution(out, dim) if out else None
        return None


class Emitter:
    def __init__(self, name, props, modules):
        self.name, self.props = name, props
        self.modules = modules                     # [Module] in Cascade's order: required, spawn, type data, others

    def module(self, cls):
        """The first module of a class ('Size', 'Color', ...), or None."""
        return next((m for m in self.modules if m.cls == cls), None)

    def all(self, cls):
        return [m for m in self.modules if m.cls == cls]

    @property
    def type_data(self):
        return next((m for m in self.modules if m.cls.startswith('TypeData')), None)


class ParticleSystem:
    def __init__(self, paks, path):
        self.path = path.split('.')[0]
        pkg = load(paks, self.path)
        objects = {pkg.object_path(i + 1): (ex.class_name, props) for i, (ex, props) in enumerate(export_properties(pkg))}
        ps = next((n for n, (c, _) in objects.items() if c == 'ParticleSystem'), None)
        if ps is None:
            raise KeyError('%s has no particle system' % path)
        self.name = re.split(r'[.:]', ps)[-1]
        self.props = objects[ps][1] or {}
        self.emitters = []
        for ref in self.props.get('Emitters') or []:
            cls, ep = objects.get(_path(ref), (None, None))
            if not ep:
                continue
            lods = ep.get('LODLevels') or []
            if not lods:
                continue
            _, lp = objects.get(_path(lods[0]), (None, {}))
            lp = lp or {}
            if lp.get('bEnabled') is False:
                continue
            mods = []
            for mref in [lp.get('RequiredModule'), lp.get('SpawnModule'), lp.get('TypeDataModule')] + \
                    list(lp.get('Modules') or []):
                if not mref:
                    continue
                mc, mp = objects.get(_path(mref), (None, None))
                if mc is None or mp is None or mp.get('bEnabled') is False:
                    continue
                mods.append(Module(mc, mp, objects))
            self.emitters.append(Emitter(ep.get('EmitterName') or re.split(r'[.:]', _path(ref))[-1], ep, mods))


class EffectMaterials(Materials):
    """Effect materials: the parameters in effect (material.Materials), the blend mode and the static switches.

    A material instance's BasePropertyOverrides apply only where their bOverride_<name> flag is set (the instance
    nearest the effect wins); otherwise the base Material's own value counts. Static switches (UseMaskTexture,
    Falloff, ...) follow the tagged properties of an instance with bHasStaticPermutationResource:
    TArray {FName name, bool32 value, bool32 overridden, FGuid}."""

    def __init__(self, paks):
        super().__init__(paks)
        self._base_blend = {}
        self._described = {}

    def _base_props(self, base_path):
        if base_path not in self._base_blend:
            props = {}
            try:
                for ex, p in export_properties(load(self.paks, base_path.split('.')[0])):
                    if ex.class_name == 'Material' and p:
                        props = p
                        break
            except KeyError:
                pass
            self._base_blend[base_path] = props
        return self._base_blend[base_path]

    def _chain(self, path):
        """[(class, properties, static switches)] from the instance to its base Material."""
        out, cur = [], path
        while cur and len(out) < 8:
            pk = load(self.paks, cur.split('.')[0])
            ex = pk.main_export()
            props, r = read_object(pk, ex)
            switches = {}
            if ex.class_name == 'MaterialInstanceConstant' and props.get('bHasStaticPermutationResource'):
                try:
                    for _ in range(r.i32()):
                        name, value, over = r.fname(), r.bool32(), r.bool32()
                        r.guid()
                        if over:
                            switches[name] = value
                except Exception:
                    pass
            out.append((ex.class_name, props, switches))
            if ex.class_name != 'MaterialInstanceConstant':
                break
            cur = short_ref(props.get('Parent'))
        return out

    def describe(self, path):
        """-> {'name', 'base', 'blend', 'two_sided', 'switches', 'textures', 'scalars', 'vectors'} for a material
        (instance)."""
        path = short_ref(path)
        if path in self._described:
            return self._described[path]
        eff = self.effective(path)
        chain = self._chain(path)
        blend = two_sided = None
        switches = {}
        for cls, props, sw in chain:                      # nearest first
            if cls == 'MaterialInstanceConstant':
                o = props.get('BasePropertyOverrides') or {}
                if blend is None and o.get('bOverride_BlendMode'):
                    blend = o.get('BlendMode')
                if two_sided is None and o.get('bOverride_TwoSided'):
                    two_sided = o.get('TwoSided')
                for k, v in sw.items():
                    switches.setdefault(k, v)
            else:
                if blend is None:
                    blend = props.get('BlendMode')
                if two_sided is None:
                    two_sided = props.get('TwoSided')
        eff['blend'] = blend if isinstance(blend, str) else 'BLEND_Opaque'
        eff['two_sided'] = bool(two_sided)
        eff['switches'] = switches
        self._described[path] = eff
        return eff


def effect_table(paks, code):
    """{effect id: particle system path} of a fighter's effect item (ECI_<CODE>_unique_0: the default costume's;
    unique_1..4 swap its hit effect for other costumes)."""
    upper = code.upper()
    try:
        props = export_properties(load(paks, '/Game/Character/Item/EffectItem/%s/ECI_%s_unique_0' % (upper, upper)))
    except KeyError:
        return {}
    out = {}
    for _, p in props:
        for item in (p or {}).get('ItemEffectMembers') or []:
            ref = short_ref(item.get('ItemEffect'))
            if ref and item.get('ItemEffectID') is not None:
                out[int(item['ItemEffectID'])] = ref
    return out


def missile_systems(paks, code):
    """{missile number: particle system path}: the missiles of a fighter's character blueprint
    (BP_Character_<CODE>: its BP_MissileManagerComponent's MissileInfo [{MissileClass, EffectID}]; a projectile
    row's field 2 is the EffectID), each missile blueprint's ParticleComp Template."""
    upper = code.upper()
    try:
        props = export_properties(load(paks, '/Game/Character/CharacterBlueprints/BP_Character_%s' % upper))
    except KeyError:
        return {}
    out = {}
    for _, p in props:
        for info in (p or {}).get('MissileInfo') or []:
            cls = short_ref(info.get('MissileClass'))
            if not cls or info.get('EffectID') is None:
                continue
            try:
                for ex, mp in export_properties(load(paks, cls.split('.')[0])):
                    if ex.class_name == 'ParticleSystemComponent' and (mp or {}).get('Template'):
                        out[int(info['EffectID'])] = short_ref(mp['Template'])
                        break
            except KeyError:
                continue
    return out
