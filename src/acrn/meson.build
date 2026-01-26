acrn_sources = files(
  'acrn_device.c',
  'acrn_domain.c',
  'acrn_driver.c',
  'acrn_manager.c',
  'acrn_monitor.c',
)

driver_source_files += acrn_sources
stateful_driver_source_files += acrn_sources

if conf.has('WITH_ACRN')
  acrn_driver_impl = static_library(
    'virt_driver_acrn_impl',
    [
      acrn_sources,
    ],
    dependencies: [
      access_dep,
      libutil_dep,
      src_dep,
    ],
    include_directories: [
      conf_inc_dir,
      hypervisor_inc_dir,
    ],
  )

  virt_modules += {
    'name': 'virt_driver_acrn',
    'link_whole': [
      acrn_driver_impl,
    ],
    'link_args': [
      libvirt_no_undefined,
    ],
  }

  virt_daemons += {
    'name': 'virtacrnd',
    'c_args': [
      '-DDAEMON_NAME="virtacrnd"',
      '-DMODULE_NAME="acrn"'
    ],
  }

  virt_conf_files += files('acrn.conf')

  virt_daemon_confs += {
    'name': 'virtacrnd',
  }

  virt_install_dirs += [
    localstatedir / 'lib' / 'libvirt' / 'acrn',
    localstatedir / 'lib' / 'libvirt' / 'acrn' / 'nvram',
  ]
endif
