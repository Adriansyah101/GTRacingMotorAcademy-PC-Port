SECTIONS
{
  .gtimg 0x00011000 (NOLOAD) :
  {
    KEEP (*(.gtimg))
  }
}
INSERT BEFORE .interp;
