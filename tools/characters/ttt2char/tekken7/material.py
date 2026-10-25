"""Tekken 7 materials and costume colours.

A MaterialInstanceConstant's tagged properties hold Parent (a Material or another instance), ScalarParameterValues,
VectorParameterValues and TextureParameterValues ({ParameterName, ParameterValue}) and BasePropertyOverrides (blend
mode, two-sidedness, ...). The values in effect are the base Material's parameter defaults (stored on its cooked
parameter expressions), overridden along the instance chain, child last.

Costumes: a costume set (CS_*) lists CustomCharacterItems (CCI_*); each CCI's ItemColors entries name a material
instance (LocationName) and give InstanceColors[0..3], which replace the parameters Color01..Color04 of that
instance for this costume (the 1P and 2P costumes are the same meshes with different CCI colours).
"""
from .uasset import export_properties, load, read_object


def short_ref(ref):
    """"Class'/Game/a/b.b'" -> '/Game/a/b.b'."""
    if ref is None:
        return None
    return ref.split("'")[1] if "'" in ref else ref


def _ref_class(ref):
    return ref.split("'")[0] if ref and "'" in ref else None


class Materials:
    def __init__(self, paks):
        self.paks = paks
        self._defaults = {}

    def _instance(self, path):
        pk = load(self.paks, path)
        ex = pk.main_export()
        props, _ = read_object(pk, ex)
        return {'path': pk.package_name + '.' + ex.name, 'cls': ex.class_name, 'parent': short_ref(props.get('Parent')),
                'parent_class': _ref_class(props.get('Parent')),
                'scalars': {p['ParameterName']: p.get('ParameterValue') for p in props.get('ScalarParameterValues', [])},
                'vectors': {p['ParameterName']: p.get('ParameterValue') for p in props.get('VectorParameterValues', [])},
                'textures': {p['ParameterName']: short_ref(p.get('ParameterValue'))
                             for p in props.get('TextureParameterValues', [])},
                'overrides': props.get('BasePropertyOverrides', {})}

    def _resolve(self, path, max_depth=8):
        chain, cur = [], path
        while cur and len(chain) < max_depth:
            m = self._instance(cur)
            chain.append(m)
            if m['cls'] != 'MaterialInstanceConstant' or m['parent_class'] != 'MaterialInstanceConstant':
                break
            cur = m['parent']
        out = {'scalars': {}, 'vectors': {}, 'textures': {}, 'overrides': {}}
        for m in reversed(chain):
            if m['cls'] == 'MaterialInstanceConstant':
                for k in out:
                    out[k].update(m[k])
        base = chain[-1]
        if base['cls'] != 'MaterialInstanceConstant':
            out['base'], out['base_class'] = base['path'], base['cls']
        else:
            out['base'], out['base_class'] = base['parent'], base['parent_class']
        return out

    def base_defaults(self, base_path):
        """Parameter defaults stored on a base Material's parameter expressions, and the textures its graph samples
        that are not parameters ('fixed': Noctis's hair-tips gradient)."""
        if base_path not in self._defaults:
            out = {'vectors': {}, 'scalars': {}, 'textures': {}, 'fixed': []}
            for ex, p in export_properties(load(self.paks, base_path.split('.')[0])):
                if p and ex.class_name == 'MaterialExpressionTextureSample' and short_ref(p.get('Texture')):
                    out['fixed'].append(short_ref(p['Texture']))
                if not p or 'ParameterName' not in p:
                    continue
                nm, cls = p['ParameterName'], ex.class_name
                if cls == 'MaterialExpressionVectorParameter':
                    out['vectors'][nm] = p.get('DefaultValue', {'r': 0.0, 'g': 0.0, 'b': 0.0, 'a': 0.0})
                elif cls == 'MaterialExpressionScalarParameter':
                    out['scalars'][nm] = p.get('DefaultValue', 0.0)
                elif 'Texture' in cls:
                    out['textures'][nm] = short_ref(p.get('Texture'))
            self._defaults[base_path] = out
        return self._defaults[base_path]

    def effective(self, path, colours=None):
        """Parameters in effect for a material instance: base defaults < instance chain < costume colours.
        -> {'name', 'base' (base material name), 'vectors', 'scalars', 'textures', 'overrides'}"""
        r = self._resolve(path)
        dfl = self.base_defaults(r['base']) if r['base_class'] == 'Material' else \
            {'vectors': {}, 'scalars': {}, 'textures': {}, 'fixed': []}
        name = path.split('.')[-1]
        eff = {'name': name, 'base': r['base'].split('.')[-1], 'overrides': r['overrides'], 'fixed': list(dfl['fixed'])}
        for k in ('vectors', 'scalars', 'textures'):
            eff[k] = dict(dfl[k])
            eff[k].update(r[k])
        if colours and name in colours:
            eff['vectors'].update(colours[name])
        return eff

    def costume_colours(self, set_path):
        """{material instance name: {'Color01': colour, ...}} of a costume set's CCIs (the last naming a material
        wins), then of the CCIs they depend on (for materials none of the set's names)."""
        cs = export_properties(load(self.paks, set_path))[0][1] or {}
        out, dependencies = {}, []
        for ref in cs.get('CustomCharacterItems', []):
            if not short_ref(ref):                  # an empty slot
                continue
            cci = export_properties(load(self.paks, short_ref(ref).split('.')[0]))[0][1] or {}
            for ic in cci.get('ItemColors', []):
                out[ic['LocationName']] = {'Color%02d' % (i + 1): c for i, c in enumerate(ic.get('InstanceColors', []))}
            dependencies += [short_ref(r).split('.')[0] for r in cci.get('DependencyItems') or [] if short_ref(r)]
        for path in dict.fromkeys(dependencies):
            cci = export_properties(load(self.paks, path))[0][1] or {}
            for ic in cci.get('ItemColors', []):
                out.setdefault(ic['LocationName'],
                               {'Color%02d' % (i + 1): c for i, c in enumerate(ic.get('InstanceColors', []))})
        return out
