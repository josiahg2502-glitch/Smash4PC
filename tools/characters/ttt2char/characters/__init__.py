"""What to take for each fighter: a module per fighter (asset paths, names and rules, no data), named in CHARACTERS.
They import nothing, so that the launcher's questions (launcher.py) need no numpy."""
import importlib

from .. import InputError

CHARACTERS = ('kazumi', 'chloe', 'eliza', 'claudio', 'akuma', 'gigas', 'josie', 'katarina', 'master_raven', 'shaheen',
              'fahkumram', 'leroy', 'lidia', 'kunimitsu2', 'negan', 'geese', 'noctis')


def definition(name):
    if name not in CHARACTERS:
        raise InputError('no fighter "%s" to import; the importer knows: %s' % (name, ', '.join(CHARACTERS)))
    return importlib.import_module('.' + name, __name__)
