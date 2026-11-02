Name:           restricto
Version:        0.1.0
Release:        0.1.alpha%{?dist}
Summary:        Open-source plasmid map and DNA sequence viewer

License:        GPL-3.0-or-later
URL:            https://github.com/mrgarbett/casin
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc-c++
BuildRequires:  cmake >= 3.16
BuildRequires:  ninja-build
BuildRequires:  pkgconfig(gtk4) >= 4.12
BuildRequires:  pkgconfig(libadwaita-1) >= 1.5
BuildRequires:  pkgconfig(cairo)
BuildRequires:  pkgconfig(pangocairo)
BuildRequires:  pkgconfig(epoxy)
BuildRequires:  desktop-file-utils
BuildRequires:  libappstream-glib

%description
Restricto views plasmids and other DNA sequences. It opens GenBank, FASTA and
SnapGene .dna files and shows a circular or linear map with features and
restriction sites next to a double-strand sequence view with translations.
Restriction enzyme data comes from REBASE.

%prep
%autosetup -n %{name}-%{version}

%build
%cmake -G Ninja -DRESTRICTO_FRONTEND=gtk
%cmake_build

%install
%cmake_install

%check
desktop-file-validate %{buildroot}%{_datadir}/applications/io.github.mrgarbett.Restricto.desktop
appstream-util validate-relax --nonet %{buildroot}%{_metainfodir}/io.github.mrgarbett.Restricto.metainfo.xml
%ctest

%files
%license README.md
%doc README.md docs/REBASE.md
%{_bindir}/restricto
%{_datadir}/applications/io.github.mrgarbett.Restricto.desktop
%{_datadir}/icons/hicolor/scalable/apps/io.github.mrgarbett.Restricto.svg
%{_datadir}/mime/packages/io.github.mrgarbett.Restricto.xml
%{_metainfodir}/io.github.mrgarbett.Restricto.metainfo.xml

%changelog
* Tue Oct 27 2026 Restricto contributors - 0.1.0-0.1.alpha
- Alpha: editing and saving, cloning simulation, primer design, read alignment
  and Sanger traces, printing, EMBL, enzyme sets, protein properties

* Tue Oct 27 2026 Restricto contributors - 0.1.0-1
- First milestone: viewer for GenBank, FASTA and .dna files
