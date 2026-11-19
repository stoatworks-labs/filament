These two files are other people's published data, not MIT, committed unedited:

- `ciexyz31_1.csv` is the CIE's: the CIE 1931 2° colour-matching functions (CIE 018:2019,
  doi:10.25039/CIE.DS.xvudnb9b), © International Commission on Illumination (CIE), under
  [CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0/), in CVRL's copy. So are the
  tables baked from it, `source/PlanckTable.h` (`tools/planck_table.py`) and
  `source/GelTable.h` (`tools/gel_table.py`).
- `lee_gels.csv` is LEE Filters' (Panavision): eight gels' spectral transmission, read from the
  graph data on LEE's colour pages (the file's header says which, and when). LEE's pages state
  no licence for it, so it is credited as LEE's published data. `source/GelTable.h` is computed
  from it too.

See [ATTRIBUTIONS.md](../../ATTRIBUTIONS.md).
