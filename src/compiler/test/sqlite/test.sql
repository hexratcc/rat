-- exercise the shell end to end, the output must stay byte stable
.mode list
.headers on
CREATE TABLE part(id INTEGER PRIMARY KEY, name TEXT NOT NULL, qty INTEGER, price REAL);
CREATE TABLE ship(id INTEGER PRIMARY KEY, part INTEGER REFERENCES part(id), n INTEGER);
CREATE INDEX part_name ON part(name);
BEGIN;
INSERT INTO part(name, qty, price) VALUES('bolt', 120, 0.25), ('nut', 340, 0.1), ('washer', 90, 0.05), ('screw', 12, 1.5);
INSERT INTO ship(part, n) VALUES(1, 10), (1, 5), (2, 40), (4, 1);
COMMIT;
SELECT name, qty, price FROM part ORDER BY name;
SELECT count(*), sum(qty), round(avg(price), 4) FROM part;
SELECT p.name, sum(s.n) AS shipped FROM part p JOIN ship s ON s.part = p.id GROUP BY p.name HAVING shipped > 4 ORDER BY p.name;
SELECT name FROM part WHERE name LIKE '%s%' ORDER BY name;
SELECT group_concat(name, '|') FROM (SELECT name FROM part ORDER BY id);
UPDATE part SET qty = qty - 10 WHERE name = 'bolt';
DELETE FROM part WHERE qty < 20;
SELECT id, name, qty FROM part ORDER BY id;
SELECT substr(name, 1, 3), upper(name), length(name) FROM part ORDER BY id;
SELECT hex(zeroblob(4)), typeof(1), typeof(1.0), typeof('x'), typeof(NULL);
WITH RECURSIVE seq(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM seq WHERE n < 10) SELECT sum(n), max(n) FROM seq;
SELECT name, qty, sum(qty) OVER (ORDER BY id) AS running FROM part ORDER BY id;
.schema part
PRAGMA integrity_check;
