str$int: procedure (p) address external;
    declare p address;
end str$int;

pr$chr: procedure (chr) external;
    declare chr byte;
end pr$chr;

pr$num: procedure (n) external;
    declare n address;
end pr$num;

pr$str: procedure (p$str) external;
    declare p$str address;
end pr$str;

newline: procedure external;
end newline;

terminate: procedure external;
end terminate;
