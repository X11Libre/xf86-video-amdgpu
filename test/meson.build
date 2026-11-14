# The checker comes with the xorg-server SDK's ABI tools.
abi_tools_dir = xorg_dep.get_variable(pkgconfig: 'abi_tools_dir',
                                      default_value: '')
if abi_tools_dir != ''
  test('private-includes',
       find_program('python3'),
       args: [abi_tools_dir / 'check-private-includes.py',
              meson.project_build_root()])
else
  warning('xorg-server SDK has no abi_tools_dir: private-includes test not built')
endif
